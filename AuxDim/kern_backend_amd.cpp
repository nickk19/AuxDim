//
//  kern_backend_amd.cpp
//  AuxDim
//
//  AMD's source backlight interface (DPCD 0x317, 0x320-0x32F), driven the way the Linux amdgpu driver drives an OLED
//  panel (edp_backlight_enable_aux and edp_set_backlight_level_nits in link_edp_panel_control.c): the interface is
//  enabled at 0x32F, then every level is written in millinits, with a transition time, at 0x320, followed by the control
//  register 0x32E = 1. Unlike the Intel backend this one switches its interface on itself, since no firmware does it
//  outside amdgpu: choosing -auxdimamd is the consent to that write. Modelled on a Linux register dump of one machine
//  (ASUS UM325U, Samsung ATNA33XC11-0) and tested on it under macOS (issue #1).
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/kern_api.hpp>

#include "kern_backend.hpp"
#include "kern_dpaux.hpp"

namespace {

	// The transition amdgpu used on that machine.
	constexpr uint16_t TransitionMs {50};

	const Backend::Register allowed[] = {
		{ Backlight::AmdLevelAddress, 6 },   // level in millinits (4 bytes LE), transition time in ms (2 bytes LE)
		{ Backlight::AmdControlAddress, 1 }, // 1: the level comes from 0x320
		{ Backlight::AmdEnableAddress, 1 },  // 1: backlight through AUX enabled
	};

	IOReturn writeByte(IOI2CInterface *aux, uint32_t address, uint8_t value) {
		return Backend::writeRegister(aux, allowed, arrsize(allowed), address, &value, 1);
	}

	bool usable(const Backlight::Caps &caps, const char *&reason) {
		if (!caps.haveAmd)
			reason = "AMD's backlight registers (DPCD 0x300-0x32F) could not be read";
		else if (!(caps.amdCaps & Backlight::AmdCapSdrAux))
			reason = "the panel does not advertise AMD brightness over AUX (DPCD 0x317 bit 0 is clear)";
		else if (!(caps.amdCaps & Backlight::AmdCapOled))
			reason = "the panel is not an OLED (DPCD 0x317 bit 4 is clear); this backend only drives OLED panels so far";
		else if (caps.vesaLuminanceControl)
			reason = "VESA luminance control is on (DPCD 0x721 bit 7), which amdgpu switches off first; not supported yet";
		else {
			reason = "usable";
			return true;
		}
		return false;
	}

	IOReturn activate(IOI2CInterface *aux, uint8_t &mode, bool &switched, bool &on) {
		switched = false;
		on = false;
		IOReturn ret = DPAux::read(aux, Backlight::AmdEnableAddress, &mode, 1);
		if (ret != kIOReturnSuccess)
			return ret;
		if ((mode & 0x01) == 0) {
			ret = writeByte(aux, Backlight::AmdEnableAddress, 1);
			if (ret != kIOReturnSuccess)
				return ret;
			switched = true;
		}
		on = true;
		return kIOReturnSuccess;
	}

	IOReturn write(IOI2CInterface *aux, uint32_t nits) {
		const uint32_t milli = nits * 1000;
		const uint8_t raw[6] = {
			static_cast<uint8_t>(milli & 0xFF), static_cast<uint8_t>((milli >> 8) & 0xFF),
			static_cast<uint8_t>((milli >> 16) & 0xFF), static_cast<uint8_t>((milli >> 24) & 0xFF),
			static_cast<uint8_t>(TransitionMs & 0xFF), static_cast<uint8_t>(TransitionMs >> 8)
		};
		IOReturn ret = Backend::writeRegister(aux, allowed, arrsize(allowed), Backlight::AmdLevelAddress, raw, sizeof(raw));
		if (ret == kIOReturnSuccess)
			ret = writeByte(aux, Backlight::AmdControlAddress, 1);
		return ret;
	}

	IOReturn read(IOI2CInterface *aux, uint32_t &nits) {
		// The current average the panel reports, which amdgpu reads back as well.
		uint8_t raw[4] = {};
		IOReturn ret = DPAux::read(aux, Backlight::AmdCurrentAddress + 4, raw, sizeof(raw));
		uint32_t milli = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8) |
			(static_cast<uint32_t>(raw[2]) << 16) | (static_cast<uint32_t>(raw[3]) << 24);
		nits = (milli + 500) / 1000;
		return ret;
	}
}

const Backend::Ops Backend::amd {
	"AMD backlight interface",
	"0x32F",
	usable,
	activate,
	write,
	read,
};
