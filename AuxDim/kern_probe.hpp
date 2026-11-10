//
//  kern_probe.hpp
//  AuxDim
//
//  Read-only hardware probe: identifies the display, the AUX transport the
//  GPU driver exposes for it and the backlight related DPCD registers, then
//  publishes the result as log lines and as properties of the plugin's
//  IOService (visible with `ioreg -rn AuxDim -l`).
//
//  The probe never writes to the panel.
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_probe_hpp
#define kern_probe_hpp

#include <IOKit/IOService.h>

#include "kern_backend.hpp"
#include "kern_edid.hpp"

namespace AuxDimProbe {

	/**
	 *  What the probe learned that the rest of the plugin acts on.
	 */
	struct Result {
		bool usable {false};       // the panel offers the backend's interface (Backend::Ops::usable)
		uint32_t edidMaxNits {0};  // EDID frame-average luminance, 0 when absent (see Policy::maxNitsFor)

		// Why usable is false. The reads failing with "offline" is what the framebuffer answers while the panel is
		// not up yet (seen right at boot), so that case is worth another try later; the others are final.
		bool retryable {false};
		const char *reason {"not probed"};
		uint32_t error {0};        // IOReturn of the failing DPCD read, when there was one
	};

	/**
	 *  Probe a display. Blocking, takes the framebuffer lock via the AUX interface,
	 *  therefore it must run on its own thread, never inside an IODisplay callback.
	 *
	 *  @param result   optional, filled with what the plugin needs to decide what to do next
	 *  @param backend  the backend result->usable is about; none in probe-only mode
	 *
	 *  @return true when the display has a native AUX interface and advertises at least one backlight
	 *          interface
	 */
	bool run(IOService *display, Result *result = nullptr, const Backend::Ops *backend = nullptr);

	/**
	 *  Log a line and expose it as a property of the plugin service, so results can be collected without
	 *  kernel logs: ioreg -rn AuxDim -l
	 */
	void publish(const char *key, const char *value);
	void publishf(const char *key, const char *format, ...) __printflike(2, 3);

	/**
	 *  Property only, no log line: for values that change with every slider move.
	 */
	void statusf(const char *key, const char *format, ...) __printflike(2, 3);

	/**
	 *  Seconds since boot, to correlate what is seen on screen with the published lines.
	 */
	uint32_t uptimeSeconds();
}

#endif /* kern_probe_hpp */
