//
//  kern_selftest.hpp
//  AuxDim
//
//  Opt-in write test (-auxdimtest): the first time the plugin writes to a panel. It answers, on real
//  hardware, which of the two backlight interfaces actually changes the luminance and what the level
//  scale looks like, before any brightness policy is built on top.
//
//  Safety rules, all enforced here or in kern_backlight.cpp:
//   - only the level registers are written (see Backlight::writeAllowed), never a mode or enable register
//   - only when the probe saw the interface advertised and, for the Intel interface, already enabled
//   - never below 10 % of the level found at the start, so the screen cannot go black
//   - the original level is written back at the end, also after an error
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_selftest_hpp
#define kern_selftest_hpp

#include <IOKit/IOService.h>

namespace AuxDimTest {

	enum class Backend {
		Intel, // DPCD 0x354 nits
		Vesa   // DPCD 0x722/0x723
	};

	/**
	 *  Run one sweep (50 %, 25 %, 10 %, original) on a display. Blocking for about 25 seconds, must run on
	 *  its own thread. Every step is published as a property of the plugin service: test-<backend>-step<N>,
	 *  test-<backend>-restore and test-<backend>.
	 */
	void run(IOService *display, Backend backend);
}

#endif /* kern_selftest_hpp */
