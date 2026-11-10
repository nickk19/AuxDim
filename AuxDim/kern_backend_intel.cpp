//
//  kern_backend_intel.cpp
//  AuxDim
//
//  Intel HDR/SDR TCON nits interface (DPCD 0x340-0x35F), as the Linux i915 driver drives it
//  (intel_dp_aux_backlight.c): the level in nits at 0x354. The interface itself is switched on by the firmware
//  (0x344 bit 4, INTEL_EDP_HDR_TCON_BRIGHTNESS_AUX_ENABLE); this backend never writes 0x344, it only checks it.
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/kern_api.hpp>

#include "kern_backend.hpp"

namespace {

	// Level LSB, MSB, then 0x356 (delay frames) and 0x357 (per frame steps): the exact transaction Linux makes.
	const Backend::Register allowed[] = {
		{ Backlight::IntelNitsAddress, 4 },
	};

	bool usable(const Backlight::Caps &caps, const char *&reason) {
		if (!caps.haveIntel)
			reason = "the Intel HDR interface (DPCD 0x340) could not be read";
		else if (!caps.intelNits)
			reason = "the panel does not advertise the Intel nits interface";
		else if (!caps.intelSdrAux)
			reason = "the panel does not accept Intel brightness over AUX";
		else if (!caps.intelNitsEnabled)
			reason = "brightness through AUX is not enabled (DPCD 0x344 bit 4 is clear)";
		else if (!caps.haveIntelLevel)
			reason = "the Intel nits level could not be read";
		else {
			reason = "usable";
			return true;
		}
		return false;
	}

	IOReturn activate(IOI2CInterface *aux, uint8_t &mode, bool &switched, bool &on) {
		switched = false;
		IOReturn ret = Backlight::readIntelControl(aux, mode);
		on = ret == kIOReturnSuccess && (mode & Backlight::IntelBrightnessAuxEnable) != 0;
		return ret;
	}

	IOReturn write(IOI2CInterface *aux, uint32_t nits) {
		// A first test with only the two level bytes was acknowledged by the panel but had no effect.
		const uint8_t raw[4] = { static_cast<uint8_t>(nits & 0xFF), static_cast<uint8_t>((nits >> 8) & 0xFF), 0, 0 };
		return Backend::writeRegister(aux, allowed, arrsize(allowed), Backlight::IntelNitsAddress, raw, sizeof(raw));
	}

	IOReturn read(IOI2CInterface *aux, uint32_t &nits) {
		uint16_t level = 0;
		IOReturn ret = Backlight::readIntel(aux, level);
		nits = level;
		return ret;
	}
}

const Backend::Ops Backend::intel {
	"Intel nits interface",
	"0x344",
	usable,
	activate,
	write,
	read,
};
