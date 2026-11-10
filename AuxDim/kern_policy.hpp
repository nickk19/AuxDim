//
//  kern_policy.hpp
//  AuxDim
//
//  The mapping from what macOS asks for to what the panel is told. Pure integer maths, no kernel
//  dependency, so it can be unit tested on any host.
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_policy_hpp
#define kern_policy_hpp

#include <Headers/kern_util.hpp>

namespace Policy {

	/**
	 *  Luminance range a maximum must lie in to be trusted. A laptop panel below 100 nits or above 1000 nits at full SDR
	 *  brightness is far more likely a decoding error than a real panel.
	 */
	static constexpr uint32_t MinMaxNits {100};
	static constexpr uint32_t MaxMaxNits {1000};

	inline bool plausibleMaxNits(uint32_t nits) {
		return nits >= MinMaxNits && nits <= MaxMaxNits;
	}

	/**
	 *  Luminance at the top of the range, or 0 when none can be trusted (the controller must then stay inert).
	 *
	 *  Same source as the Linux driver (drm_calculate_luminance_range, intel_dp_aux_hdr_setup_backlight): the EDID
	 *  frame-average luminance (CTA-861 HDR static metadata). Linux does not use the Intel interface at all without
	 *  that metadata; here the user can supply the value instead.
	 *
	 *  @param edidAverageNits EDID frame-average luminance, 0 when absent
	 *  @param overrideNits    auxdimmaxnits boot argument, 0 when not set
	 */
	inline uint32_t maxNitsFor(uint32_t edidAverageNits, uint32_t overrideNits) {
		if (overrideNits)
			return plausibleMaxNits(overrideNits) ? overrideNits : 0;
		return plausibleMaxNits(edidAverageNits) ? edidAverageNits : 0;
	}

	/**
	 *  Lowest luminance ever sent: the requested floor (0 when none was given, the panel's darkest, as on a Mac),
	 *  always well below the top of the range.
	 */
	inline uint32_t floorNitsFor(uint32_t requestedFloor, uint32_t maxNits) {
		return requestedFloor < maxNits ? requestedFloor : maxNits / 4;
	}

	/**
	 *  Panel luminance in nits for a display brightness value.
	 *
	 *  The result always lies in [floorNits, maxNits].
	 *
	 *  - "linear-brightness" is proportional to luminance: nits = floor + (max - floor) * f
	 *  - "brightness" is the perceptual slider position: a square law (gamma 2) approximates it,
	 *    nits = floor + (max - floor) * f^2
	 *
	 *  where f = (value - min) / (max - min).
	 *
	 *  @param value     requested value, clamped to [min, max]
	 *  @param min       lowest value of the display parameter
	 *  @param max       highest value of the display parameter
	 *  @param linear    true for the linear-brightness parameter
	 *  @param floorNits lowest luminance ever sent
	 *  @param maxNits   luminance at the top of the range
	 */
	inline uint32_t nitsFor(uint32_t value, uint32_t min, uint32_t max, bool linear, uint32_t floorNits, uint32_t maxNits) {
		if (maxNits <= floorNits || max <= min)
			return maxNits;

		if (value < min)
			value = min;
		if (value > max)
			value = max;

		uint64_t num = value - min;
		uint64_t den = max - min;

		// Keep num * num below 2^64 whatever range the parameter has.
		while (den > 0xFFFF) {
			den >>= 1;
			num >>= 1;
		}
		if (den == 0)
			return maxNits;

		uint64_t span = maxNits - floorNits;
		uint64_t scaled = linear
			? (span * num + den / 2) / den
			: (span * num * num + (den * den) / 2) / (den * den);

		return floorNits + static_cast<uint32_t>(scaled);
	}
}

#endif /* kern_policy_hpp */
