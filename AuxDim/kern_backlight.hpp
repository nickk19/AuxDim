//
//  kern_backlight.hpp
//  AuxDim
//
//  The DPCD backlight interfaces an eDP panel can implement, as plain register access:
//
//   - VESA eDP "AUX set" backlight, DPCD 0x720-0x723 (16 bit level, or 8 bit)
//   - Intel HDR/SDR TCON "nits" brightness, DPCD 0x340-0x35F, level at 0x354/0x355
//   - AMD's source backlight registers, DPCD 0x317 (panel caps) and 0x320-0x32F, level in millinits at 0x320
//
//  Register layouts follow the public Linux sources (drm_dp_helper.h, intel_dp_aux_backlight.c, and for AMD
//  dpcd_defs.h, dc_dp_types.h and link_edp_panel_control.c of amdgpu).
//  Reads only: the writes belong to the backends (kern_backend.hpp), each with its own list of allowed registers.
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_backlight_hpp
#define kern_backlight_hpp

#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <IOKit/i2c/IOI2CInterface.h>

namespace Backlight {

	static constexpr uint32_t IntelCapsAddress       {0x340}; // version, caps 1, caps 2, caps 3, control (0x344)
	static constexpr uint32_t IntelControlAddress    {0x344}; // mode bits, see IntelBrightnessAuxEnable
	static constexpr uint32_t IntelNitsAddress       {0x354}; // level LSB, MSB, delay frames, per frame steps
	static constexpr uint32_t VesaEdpAddress         {0x700}; // eDP revision, general caps, backlight adjustment caps
	static constexpr uint32_t VesaControlAddress     {0x720}; // display control, mode set, level MSB (0x722), LSB (0x723)
	static constexpr uint32_t VesaBrightnessAddress  {0x722};
	static constexpr uint32_t SourceOuiAddress       {0x300}; // OUI of the GPU driver that last wrote it (AMD 00 00 1a, Intel 00 aa 01)
	static constexpr uint32_t AmdSinkCapsAddress     {0x317}; // see AmdCap*
	static constexpr uint32_t AmdLevelAddress        {0x320}; // level in millinits (4 bytes LE), transition time in ms (2 bytes LE)
	static constexpr uint32_t AmdCurrentAddress      {0x326}; // current peak (4 bytes LE) and average (0x32A, 4 bytes LE), millinits
	static constexpr uint32_t AmdControlAddress      {0x32E}; // 1: the level comes from 0x320 (amdgpu sets it for OLED and HDR)
	static constexpr uint32_t AmdEnableAddress       {0x32F}; // 1: backlight through AUX enabled

	// 0x317 bits (union dpcd_sink_ext_caps in amdgpu)
	static constexpr uint8_t AmdCapSdrAux  {0x01};
	static constexpr uint8_t AmdCapHdrAux  {0x02};
	static constexpr uint8_t AmdCapOled    {0x10};
	static constexpr uint8_t AmdCapMiniLed {0x40};

	// 0x344 bit 4 (INTEL_EDP_HDR_TCON_BRIGHTNESS_AUX_ENABLE in Linux): the panel takes its level from 0x354. While it is clear
	// the level register is ignored.
	static constexpr uint8_t IntelBrightnessAuxEnable {0x10};

	/**
	 *  What a panel advertises and what is currently programmed. Every field is "false/0" when the
	 *  matching register range could not be read.
	 */
	struct Caps {
		// Intel nits interface
		bool haveIntel {false};
		uint8_t intelVersion {0};
		bool intelNits {false};         // version >= 1 and the nits brightness capability bit
		bool intelSdrAux {false};       // SDR brightness is set through AUX (and not through the PWM pin)
		uint8_t intelControl {0};       // 0x344
		bool intelNitsEnabled {false};  // 0x344 bit 4 (IntelBrightnessAuxEnable): the panel takes its level from 0x354
		bool haveIntelLevel {false};
		uint16_t intelLevel {0};        // 0x354/0x355

		// VESA eDP interface
		bool haveVesa {false};
		uint8_t edpRevision {0};        // 0x700: 0 = 1.1 ... 3 = 1.4, 5 = 1.4b
		bool vesaAux {false};           // backlight adjustment by DPCD is advertised
		bool vesaPwmPin {false};
		bool vesaTwoBytes {false};      // 16 bit level, otherwise 8 bit
		bool vesaEnabled {false};       // 0x720 bit 0
		uint8_t vesaMode {0};           // 0x721 bits 0-1: 0 pwm pin, 1 preset, 2 dpcd, 3 product
		bool vesaLuminanceControl {false}; // 0x721 bit 7: VESA panel luminance control (ABC)
		bool haveVesaLevel {false};
		uint16_t vesaLevel {0};

		// Source OUI and AMD interface (0x300-0x32F)
		bool haveAmd {false};
		uint8_t sourceOui[3] {};
		uint8_t amdCaps {0};            // 0x317
		uint32_t amdLevel {0};          // 0x320, millinits
		uint16_t amdTransitionMs {0};   // 0x324
		uint32_t amdPeak {0};           // 0x326, millinits
		uint32_t amdAverage {0};        // 0x32A, millinits
		uint8_t amdControl {0};         // 0x32E
		uint8_t amdEnable {0};          // 0x32F
	};

	/**
	 *  Read every interface. Returns success when at least one of them could be read.
	 */
	IOReturn readCaps(IOI2CInterface *aux, Caps &caps);

	/**
	 *  Level and control registers. The VESA level is 16 bit when twoBytes is set, 8 bit otherwise.
	 */
	IOReturn readIntel(IOI2CInterface *aux, uint16_t &level);
	IOReturn readIntelControl(IOI2CInterface *aux, uint8_t &control);
	IOReturn readVesa(IOI2CInterface *aux, bool twoBytes, uint16_t &level);
}

#endif /* kern_backlight_hpp */
