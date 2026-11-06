//
//  kern_probe.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

#include <IOKit/graphics/IOGraphicsTypes.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>
#include <stdarg.h>

#include "kern_probe.hpp"
#include "kern_dpaux.hpp"
#include "kern_edid.hpp"
#include "kern_backlight.hpp"

void AuxDimProbe::publish(const char *key, const char *value) {
	SYSLOG("auxdim", "probe: %s = %s", key, value);
	if (ADDPR(selfInstance))
		ADDPR(selfInstance)->setProperty(key, value);
}

void AuxDimProbe::publishf(const char *key, const char *format, ...) {
	char line[240];
	va_list va;
	va_start(va, format);
	vsnprintf(line, sizeof(line), format, va);
	va_end(va);
	publish(key, line);
}

uint32_t AuxDimProbe::uptimeSeconds() {
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return static_cast<uint32_t>(ns / 1000000000ULL);
}

namespace {

	using AuxDimProbe::publish;
	using AuxDimProbe::publishf;

	void toHex(char *dst, size_t dstSize, const uint8_t *src, size_t len) {
		static const char digits[] = "0123456789abcdef";
		size_t pos = 0;
		for (size_t i = 0; i < len && pos + 3 < dstSize; i++) {
			dst[pos++] = digits[src[i] >> 4];
			dst[pos++] = digits[src[i] & 0xF];
			if (i + 1 < len)
				dst[pos++] = ' ';
		}
		dst[pos] = '\0';
	}

	/**
	 *  @return true when the EDID carries HDR luminance data, copied to lum
	 */
	bool describeEdid(IOService *display, EDID::Luminance &lum) {
		OSObject *obj = display->copyProperty(kIODisplayEDIDKey);
		auto *edid = OSDynamicCast(OSData, obj);
		if (!edid || edid->getLength() < 128) {
			publish("edid", "unavailable");
			OSSafeReleaseNULL(obj);
			return false;
		}

		auto *b = static_cast<const uint8_t *>(edid->getBytesNoCopy());
		char line[160];

		// Manufacturer ID: three 5-bit letters packed big endian in bytes 8-9.
		char mfg[4] = {
			static_cast<char>('A' - 1 + ((b[8] >> 2) & 0x1F)),
			static_cast<char>('A' - 1 + (((b[8] & 0x3) << 3) | (b[9] >> 5))),
			static_cast<char>('A' - 1 + (b[9] & 0x1F)),
			'\0'
		};
		uint32_t product = static_cast<uint32_t>(b[10]) | (static_cast<uint32_t>(b[11]) << 8);

		// Monitor name (0xFC) or, for panels that only carry a model code, the data string (0xFE).
		char name[14] = {};
		EDID::name(b, edid->getLength(), name, sizeof(name));

		snprintf(line, sizeof(line), "mfg=%s product=0x%04x name=\"%s\" edid=%u.%u length=%u",
				 mfg, product, name, b[18], b[19], edid->getLength());
		publish("edid", line);

		bool haveLum = EDID::hdrLuminance(b, edid->getLength(), lum);
		if (haveLum) {
			publishf("edid-luminance", "peak=%u nits average=%u nits min=%u.%03u nits (CTA-861 HDR static metadata)",
					 lum.peak, lum.average, lum.minMilli / 1000, lum.minMilli % 1000);
		} else {
			publish("edid-luminance", "absent");
		}

		// The raw blocks are the most useful thing to attach to a bug report.
		char hex[400];
		for (uint32_t block = 0; block < 4 && (block + 1) * 128 <= edid->getLength(); block++) {
			char key[24];
			snprintf(key, sizeof(key), "edid-block-%u", block);
			toHex(hex, sizeof(hex), b + block * 128, 128);
			publish(key, hex);
		}

		OSSafeReleaseNULL(obj);
		return haveLum;
	}

	void describeParameters(IOService *display) {
		OSObject *obj = display->copyProperty(kIODisplayParametersKey);
		auto *params = OSDynamicCast(OSDictionary, obj);
		if (!params) {
			publish("display-parameters", "absent");
			OSSafeReleaseNULL(obj);
			return;
		}

		static const char *keys[] = { kIODisplayBrightnessKey, kIODisplayLinearBrightnessKey };
		for (auto name : keys) {
			char key[48];
			snprintf(key, sizeof(key), "param-%s", name);
			auto *entry = OSDynamicCast(OSDictionary, params->getObject(name));
			if (!entry) {
				publish(key, "absent");
				continue;
			}
			auto *min = OSDynamicCast(OSNumber, entry->getObject(kIODisplayMinValueKey));
			auto *max = OSDynamicCast(OSNumber, entry->getObject(kIODisplayMaxValueKey));
			auto *val = OSDynamicCast(OSNumber, entry->getObject(kIODisplayValueKey));
			char line[80];
			snprintf(line, sizeof(line), "min=%d max=%d value=%d",
					 min ? static_cast<int>(min->unsigned32BitValue()) : -1,
					 max ? static_cast<int>(max->unsigned32BitValue()) : -1,
					 val ? static_cast<int>(val->unsigned32BitValue()) : -1);
			publish(key, line);
		}

		OSSafeReleaseNULL(obj);
	}

	struct DpcdRange {
		uint32_t address;
		uint32_t length;
	};

	// All reads. Keep this list in sync with Docs/PROBING.md.
	//   0x000  receiver capability (DPCD revision, link rate, lane count)
	//   0x300  source OUI as last written by the GPU driver
	//   0x340  Intel HDR / SDR TCON capabilities, control and nits brightness (0x354)
	//   0x400  sink OUI and device identification string
	//   0x700  eDP capability (revision, general capability, backlight adjustment capability)
	//   0x720  eDP backlight control (display control, mode, brightness MSB/LSB, PWM generator)
	const DpcdRange dpcdRanges[] = {
		{ 0x000, 16 },
		{ 0x300, 16 },
		{ 0x340, 32 },
		{ 0x400, 16 },
		{ 0x700, 16 },
		{ 0x720, 16 },
	};

	void dumpDpcd(IOI2CInterface *aux) {
		for (const auto &range : dpcdRanges) {
			uint8_t buffer[32] = {};
			char key[24], hex[128];
			snprintf(key, sizeof(key), "dpcd-%04x", range.address);

			IOReturn ret = DPAux::read(aux, range.address, buffer, range.length);
			if (ret != kIOReturnSuccess) {
				char err[32];
				snprintf(err, sizeof(err), "read failed 0x%x", ret);
				publish(key, err);
				continue;
			}

			toHex(hex, sizeof(hex), buffer, range.length);
			publish(key, hex);
		}
	}

	const char *vesaModeName(uint8_t mode) {
		switch (mode) {
			case 0:  return "pwm-pin";
			case 1:  return "preset";
			case 2:  return "dpcd";
			default: return "product";
		}
	}

	const char *yesNo(bool value) {
		return value ? "yes" : "no";
	}

	/**
	 *  Decoded view of the two backlight interfaces (cf. Linux drm_dp_helper.h and intel_dp_aux_backlight.c).
	 *  The raw dpcd-XXXX bytes stay authoritative.
	 */
	void describeCaps(const Backlight::Caps &caps) {
		if (caps.haveIntel) {
			publishf("intel-state",
					 "version=%u nits-capable=%s sdr-via-aux=%s control=0x%02x nits-enabled=%s level=%u",
					 caps.intelVersion, yesNo(caps.intelNits), yesNo(caps.intelSdrAux), caps.intelControl,
					 yesNo(caps.intelNitsEnabled), caps.haveIntelLevel ? caps.intelLevel : 0);
		} else {
			publish("intel-state", "unreadable");
		}

		if (caps.haveVesa) {
			publishf("vesa-state",
					 "edp-revision=0x%02x aux-brightness=%s pwm-pin=%s two-bytes=%s enabled=%s mode=%u(%s) level=0x%04x",
					 caps.edpRevision, yesNo(caps.vesaAux), yesNo(caps.vesaPwmPin), yesNo(caps.vesaTwoBytes),
					 yesNo(caps.vesaEnabled), caps.vesaMode, vesaModeName(caps.vesaMode),
					 caps.haveVesaLevel ? caps.vesaLevel : 0);
		} else {
			publish("vesa-state", "unreadable");
		}

		publishf("hint-backlight", "vesa-aux-set:%s vesa-pwm-pin:%s vesa-mode:%s intel-hdr-nits:%s intel-sdr-aux:%s",
				 caps.haveVesa ? yesNo(caps.vesaAux) : "?",
				 caps.haveVesa ? yesNo(caps.vesaPwmPin) : "?",
				 caps.haveVesa ? vesaModeName(caps.vesaMode) : "?",
				 caps.haveIntel ? yesNo(caps.intelNits) : "?",
				 caps.haveIntel ? yesNo(caps.intelSdrAux) : "?");
	}
}

bool AuxDimProbe::run(IOService *display, Result *result) {
	if (!display)
		return false;
	if (result)
		*result = Result();

	char line[160];
	publish("display-class", display->getMetaClass()->getClassName());
	snprintf(line, sizeof(line), "%s", display->getName());
	publish("display-name", line);

	EDID::Luminance lum;
	bool haveLum = describeEdid(display, lum);
	describeParameters(display);

	IOService *framebuffer = DPAux::copyFramebuffer(display);
	if (!framebuffer) {
		publish("framebuffer", "not found");
		if (result)
			result->reason = "no framebuffer found for this display";
		return false;
	}
	publish("framebuffer-class", framebuffer->getMetaClass()->getClassName());

	DPAux::Interface list[8];
	size_t count = DPAux::collectInterfaces(framebuffer, list, 8);
	snprintf(line, sizeof(line), "%u", static_cast<unsigned>(count));
	publish("aux-interface-count", line);

	IOI2CInterface *aux = nullptr;
	for (size_t i = 0; i < count; i++) {
		char key[32];
		snprintf(key, sizeof(key), "aux-interface-%u", static_cast<unsigned>(i));
		snprintf(line, sizeof(line), "id=0x%llx bus-type=%u transaction-types=0x%x native-aux=%s",
				 list[i].id, list[i].busType, list[i].txTypes,
				 list[i].supportsNativeAux() ? "yes" : "no");
		publish(key, line);
		if (!aux && list[i].supportsNativeAux())
			aux = list[i].service;
	}

	bool writable = false;
	if (aux) {
		dumpDpcd(aux);

		Backlight::Caps caps;
		if (Backlight::readCaps(aux, caps) == kIOReturnSuccess) {
			describeCaps(caps);
			bool intelUsable = caps.intelNits && caps.intelSdrAux && caps.intelNitsEnabled && caps.haveIntelLevel;
			writable = intelUsable || caps.vesaAux;

			if (result) {
				if (intelUsable)
					result->reason = "usable";
				else if (!caps.haveIntel)
					result->reason = "the Intel HDR interface (DPCD 0x340) could not be read";
				else if (!caps.intelNits)
					result->reason = "the panel does not advertise the Intel nits interface";
				else if (!caps.intelSdrAux)
					result->reason = "the panel does not accept Intel brightness over AUX";
				else if (!caps.intelNitsEnabled)
					result->reason = "the Intel nits mode is not enabled (DPCD 0x344 bit 4 is clear)";
				else
					result->reason = "the Intel nits level could not be read";

				result->writable = writable;
				result->intelUsable = intelUsable;
				// The panel reads back one nit less than it was given, so the level found at boot (399) stands for
				// the EDID's 400. Trust the EDID when it is plausible, otherwise the level plus that one nit.
				result->maxNits = (haveLum && lum.average >= 100 && lum.average <= 1000)
					? lum.average : static_cast<uint32_t>(caps.intelLevel) + 1;
			}
		} else if (result) {
			// Nothing answered. Typically kIOReturnOffline (0xe00002d7) while the panel is not up yet.
			uint8_t scratch = 0;
			result->error = static_cast<uint32_t>(DPAux::read(aux, Backlight::IntelCapsAddress, &scratch, 1));
			result->reason = "the panel did not answer any DPCD read";
			result->retryable = true;
		}
	} else {
		publish("dpcd", "no DisplayPort-native interface exposed by the framebuffer driver");
		if (result)
			result->reason = "the framebuffer exposes no DisplayPort-native AUX interface";
	}

	DPAux::releaseInterfaces(list, count);
	framebuffer->release();
	return writable;
}
