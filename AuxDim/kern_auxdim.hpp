//
//  kern_auxdim.hpp
//  AuxDim
//
//  With -auxdimprobe or a backend argument (-auxdimintel, -auxdimamd, see kern_backend.hpp): watches the display parameters
//  macOS sends to every IODisplay, probes the display hardware (implied by a backend) and applies the brightness
//  macOS asks for through the backend (see kern_control.hpp). Without either it does nothing.
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_auxdim_hpp
#define kern_auxdim_hpp

#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <kern/thread_call.h>

#include "kern_control.hpp"

class AuxDimCore {
public:
	/**
	 *  There is no teardown: once a Lilu plugin has started it is never unloaded (kern_stop in Lilu's plugin_start.cpp
	 *  refuses), so the hook, the thread calls and the retained display live until shutdown.
	 */
	void init();

private:
	/**
	 *  Private self instance for callbacks
	 */
	static AuxDimCore *callbackAuxDim;

	/**
	 *  Called for every kext Lilu reports, we only care about IOGraphicsFamily
	 */
	void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);

	/**
	 *  Original IODisplay::doIntegerSet trampoline
	 *
	 *  IODisplay::setProperties() -> setForKey() -> doIntegerSet() is the single funnel through which
	 *  every display parameter (brightness included) passes, whatever the GPU driver is, so hooking it
	 *  keeps the plugin independent from the video stack.
	 */
	mach_vm_address_t orgDoIntegerSet {};

	/**
	 *  IODisplay::doIntegerSet wrapper. Runs with the framebuffer lock held.
	 */
	static bool wrapDoIntegerSet(void *that, OSDictionary *params, const OSSymbol *name, UInt32 value);

	/**
	 *  Per-parameter bookkeeping, must stay cheap and must not block.
	 */
	void handleParameter(IOService *display, OSDictionary *params, const OSSymbol *name, UInt32 value, bool handled);

	/**
	 *  Deferred probe, see kern_probe.hpp. Disabled unless -auxdimprobe is passed.
	 */
	static constexpr size_t MaxProbedDisplays {8};
	static constexpr uint32_t ProbeDelayMs {3000};

	// While the panel does not answer (offline at boot) the probe is repeated: first quickly, then slowly.
	static constexpr uint32_t RetryFastMs {5000};
	static constexpr uint32_t RetrySlowMs {30000};
	static constexpr uint32_t RetryFastCount {12};
	static constexpr uint32_t RetryMaxCount {40};
	uint32_t ctlAttempts {0};
	bool probeEnabled {false};
	volatile uint32_t probeBusy {0};
	uint64_t probed[MaxProbedDisplays] {};  // registry entry IDs
	size_t probedCount {0};
	IOService *probeDisplay {nullptr};
	thread_call_t probeCall {nullptr};

	void scheduleProbe(IOService *display);
	static void probeEntry(thread_call_param_t param0, thread_call_param_t param1);

	/**
	 *  Brightness control, enabled by a backend argument. Needs the probe to have found the backend's interface usable;
	 *  until then requests are only remembered.
	 */
	bool ctlEnabled {false};
	const Backend::Ops *ctlBackend {nullptr};
	char ctlConflict[64] {};          // the backend arguments, when more than one was given
	uint32_t ctlMaxNitsOverride {0}; // auxdimmaxnits=N, 0 when not set
	uint32_t ctlRangeMisses {0}; // brightness requests that carried no usable min/max
	Controller controller;

	/**
	 *  Observed brightness traffic. The hook only counts; a delayed thread call publishes the counters as
	 *  properties (brightness-events, brightness-last), which proves the input path end to end without logs.
	 */
	uint32_t brightnessEvents {0};
	uint32_t lastBrightness {0};
	uint32_t lastLinearBrightness {0};
	uint32_t powerEvents {0};   // dsyp changes, see Controller::powerState
	uint32_t lastPower {0};
	thread_call_t statusCall {nullptr};

	void scheduleStatus();
	static void statusEntry(thread_call_param_t param0, thread_call_param_t param1);
};

#endif /* kern_auxdim_hpp */
