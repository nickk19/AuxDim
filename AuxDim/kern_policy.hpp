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
	 *  Panel luminance in nits for a display brightness value.
	 *
	 *  The result always lies in [floorNits, maxNits], so no slider position can produce a black screen.
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
