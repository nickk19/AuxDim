//
//  kern_backlight.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/kern_api.hpp>

#include "kern_backlight.hpp"
#include "kern_dpaux.hpp"

namespace {

	/**
	 *  The single gate for writes. Only the Intel level block passes: 0x354...0x357 (level LSB, MSB, then the
	 *  transition delay and step registers, written as zero), the exact transaction the Linux driver makes.
	 */
	bool writeAllowed(uint32_t address, uint32_t length) {
		return address == Backlight::IntelNitsAddress && length == 4;
	}

	IOReturn guardedWrite(IOI2CInterface *aux, uint32_t address, const uint8_t *data, uint32_t length) {
		if (!writeAllowed(address, length)) {
			SYSLOG("auxdim", "refusing DPCD write to 0x%x (%u bytes)", address, length);
			return kIOReturnNotPermitted;
		}
		return DPAux::write(aux, address, data, length);
	}
}

IOReturn Backlight::readIntel(IOI2CInterface *aux, uint16_t &level) {
	uint8_t raw[2] = {};
	IOReturn ret = DPAux::read(aux, IntelNitsAddress, raw, sizeof(raw));
	if (ret == kIOReturnSuccess)
		level = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
	return ret;
}

IOReturn Backlight::readIntelControl(IOI2CInterface *aux, uint8_t &control) {
	return DPAux::read(aux, IntelControlAddress, &control, 1);
}

IOReturn Backlight::writeIntel(IOI2CInterface *aux, uint16_t level) {
	// Same layout the Linux driver writes (intel_dp_aux_hdr_set_aux_backlight): level LSB, MSB, then 0x356
	// (delay frames) and 0x357 (per frame steps) as zero, in a single 4 byte transaction. A first test with
	// only the two level bytes was acknowledged by the panel but had no effect.
	const uint8_t raw[4] = { static_cast<uint8_t>(level & 0xFF), static_cast<uint8_t>(level >> 8), 0, 0 };
	return guardedWrite(aux, IntelNitsAddress, raw, sizeof(raw));
}

IOReturn Backlight::readVesa(IOI2CInterface *aux, bool twoBytes, uint16_t &level) {
	uint8_t raw[2] = {};
	uint32_t count = twoBytes ? 2 : 1;
	IOReturn ret = DPAux::read(aux, VesaBrightnessAddress, raw, count);
	if (ret == kIOReturnSuccess)
		level = twoBytes ? static_cast<uint16_t>((raw[0] << 8) | raw[1]) : raw[0];
	return ret;
}

IOReturn Backlight::readCaps(IOI2CInterface *aux, Caps &caps) {
	caps = Caps();
	if (!aux)
		return kIOReturnBadArgument;

	uint8_t intel[8] = {}; // 0x340...0x347
	if (DPAux::read(aux, IntelCapsAddress, intel, sizeof(intel)) == kIOReturnSuccess) {
		caps.haveIntel = true;
		caps.intelVersion = intel[0];
		caps.intelNits = intel[0] >= 1 && (intel[1] & 0x10) != 0;
		caps.intelSdrAux = (intel[2] & 0x01) != 0;
		caps.intelControl = intel[4];
		caps.intelNitsEnabled = (intel[4] & IntelBrightnessAuxEnable) != 0;
		if (caps.intelNits)
			caps.haveIntelLevel = readIntel(aux, caps.intelLevel) == kIOReturnSuccess;
	}

	uint8_t edp[3] = {}; // 0x700...0x702
	if (DPAux::read(aux, VesaEdpAddress, edp, sizeof(edp)) == kIOReturnSuccess) {
		caps.haveVesa = true;
		caps.edpRevision = edp[0];
		caps.vesaAux = (edp[1] & 0x01) != 0 && (edp[2] & 0x02) != 0;
		caps.vesaPwmPin = (edp[2] & 0x01) != 0;
		caps.vesaTwoBytes = (edp[2] & 0x04) != 0;
	}

	uint8_t ctl[2] = {}; // 0x720, 0x721
	if (caps.haveVesa && DPAux::read(aux, VesaControlAddress, ctl, sizeof(ctl)) == kIOReturnSuccess) {
		caps.vesaEnabled = (ctl[0] & 0x01) != 0;
		caps.vesaMode = ctl[1] & 0x03;
		if (caps.vesaAux)
			caps.haveVesaLevel = readVesa(aux, caps.vesaTwoBytes, caps.vesaLevel) == kIOReturnSuccess;
	}

	return (caps.haveIntel || caps.haveVesa) ? kIOReturnSuccess : kIOReturnIOError;
}
