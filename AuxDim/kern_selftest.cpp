//
//  kern_selftest.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/kern_api.hpp>

#include <IOKit/IOLib.h>

#include "kern_selftest.hpp"
#include "kern_probe.hpp"
#include "kern_dpaux.hpp"
#include "kern_backlight.hpp"

namespace {

	// Fractions of the starting level, in percent. The lowest one is the floor.
	const uint8_t sweepPercent[] = { 50, 25, 10 };

	// Below this a percentage of the level is meaningless (or the backlight is simply off).
	constexpr uint16_t MinStartLevel = 20;

	constexpr uint32_t HoldMs    {3000};
	constexpr uint32_t ReadBackMs {1000};

	struct Target {
		AuxDimTest::Backend backend;
		IOI2CInterface *aux;
		bool vesaTwoBytes;

		IOReturn read(uint16_t &level) const {
			return backend == AuxDimTest::Backend::Intel ? Backlight::readIntel(aux, level)
														: Backlight::readVesa(aux, vesaTwoBytes, level);
		}

		IOReturn write(uint16_t level) const {
			return backend == AuxDimTest::Backend::Intel ? Backlight::writeIntel(aux, level)
														: Backlight::writeVesa(aux, vesaTwoBytes, level);
		}
	};
}

void AuxDimTest::run(IOService *display, Backend backend) {
	const char *tag = backend == Backend::Intel ? "intel" : "vesa";
	char summary[24];
	snprintf(summary, sizeof(summary), "test-%s", tag);

	IOI2CInterface *aux = DPAux::copyNativeAux(display);
	if (!aux) {
		AuxDimProbe::publish(summary, "skipped: no native AUX interface");
		return;
	}

	Backlight::Caps caps;
	Backlight::readCaps(aux, caps);

	bool usable = backend == Backend::Intel
		? (caps.intelNits && caps.intelSdrAux && caps.intelNitsEnabled && caps.haveIntelLevel)
		: (caps.vesaAux && caps.haveVesaLevel);
	if (!usable) {
		AuxDimProbe::publish(summary, "skipped: interface not advertised, not enabled or unreadable (see intel-state, vesa-state)");
		aux->release();
		return;
	}

	Target target { backend, aux, caps.vesaTwoBytes };
	const uint16_t original = backend == Backend::Intel ? caps.intelLevel : caps.vesaLevel;
	if (original < MinStartLevel) {
		AuxDimProbe::publishf(summary, "skipped: starting level %u is too low to scale safely", original);
		aux->release();
		return;
	}

	AuxDimProbe::publishf(summary, "started at uptime %us, starting level %u", AuxDimProbe::uptimeSeconds(), original);

	bool failed = false;
	for (size_t i = 0; i < arrsize(sweepPercent); i++) {
		uint16_t level = static_cast<uint16_t>(static_cast<uint32_t>(original) * sweepPercent[i] / 100);
		if (level == 0)
			level = 1;

		IOReturn writeRet = target.write(level);
		IOSleep(ReadBackMs);

		uint16_t readBack = 0;
		IOReturn readRet = target.read(readBack);

		char key[32];
		snprintf(key, sizeof(key), "test-%s-step%u", tag, static_cast<unsigned>(i));
		AuxDimProbe::publishf(key, "uptime %us: wrote %u (%u%%) ret 0x%x, read back %u ret 0x%x",
							 AuxDimProbe::uptimeSeconds(), level, sweepPercent[i], writeRet, readBack, readRet);

		// Anything else that moved in the Intel block (0x340...0x35F) is as interesting as the level itself.
		if (backend == AuxDimTest::Backend::Intel) {
			uint8_t block[32] = {};
			if (DPAux::read(aux, Backlight::IntelCapsAddress, block, sizeof(block)) == kIOReturnSuccess) {
				char hex[100];
				size_t pos = 0;
				for (size_t b = 0; b < sizeof(block) && pos + 3 < sizeof(hex); b++)
					pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x%s", block[b], b + 1 < sizeof(block) ? " " : "");
				snprintf(key, sizeof(key), "test-intel-step%u-dpcd", static_cast<unsigned>(i));
				AuxDimProbe::publish(key, hex);
			}
		}

		if (writeRet != kIOReturnSuccess || readRet != kIOReturnSuccess) {
			failed = true;
			break;
		}

		IOSleep(HoldMs);
	}

	// Always put the starting level back.
	IOReturn writeRet = target.write(original);
	IOSleep(ReadBackMs / 2);
	uint16_t readBack = 0;
	IOReturn readRet = target.read(readBack);
	// The Intel panel reads back one nit less than it was given (399 written, 398 read), so allow that.
	uint16_t difference = readBack > original ? readBack - original : original - readBack;
	if (writeRet != kIOReturnSuccess || readRet != kIOReturnSuccess || difference > 1)
		failed = true;

	char key[32];
	snprintf(key, sizeof(key), "test-%s-restore", tag);
	AuxDimProbe::publishf(key, "uptime %us: wrote %u ret 0x%x, read back %u ret 0x%x",
						 AuxDimProbe::uptimeSeconds(), original, writeRet, readBack, readRet);

	AuxDimProbe::publish(summary, failed ? "finished with errors, restore attempted (a reboot also resets the panel)"
										: "finished, starting level restored");
	aux->release();
}
