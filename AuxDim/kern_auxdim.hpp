//
//  kern_auxdim.hpp
//  AuxDim
//
//  Phase 0: observe the brightness requests macOS sends to every IODisplay and,
//  on demand, probe the display hardware. No brightness is applied yet.
//  Phase 0.5 (-auxdimtest): a self-reverting write test of the panel's backlight registers.
//  Phase 1 (-auxdimctl): apply the brightness macOS asks for, see kern_control.hpp.
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
	void init();
	void deinit();

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
	static constexpr size_t MaxProbedDisplays {4};
	static constexpr uint32_t ProbeDelayMs {3000};

	// While the panel does not answer (offline at boot) the probe is repeated: first quickly, then slowly.
	static constexpr uint32_t RetryFastMs {5000};
	static constexpr uint32_t RetrySlowMs {30000};
	static constexpr uint32_t RetryFastCount {12};
	static constexpr uint32_t RetryMaxCount {40};
	uint32_t ctlAttempts {0};
	bool probeEnabled {false};
	volatile uint32_t probeBusy {0};
	uintptr_t probed[MaxProbedDisplays] {};
	size_t probedCount {0};
	IOService *probeDisplay {nullptr};
	thread_call_t probeCall {nullptr};

	void scheduleProbe(IOService *display);
	static void probeEntry(thread_call_param_t param0, thread_call_param_t param1);

	/**
	 *  Opt-in write test, see kern_selftest.hpp. Armed after a probe found a usable backlight interface,
	 *  then a sweep runs on the first such display: stage 0 the Intel nits interface and, with -auxdimtestvesa,
	 *  stage 1 (a minute later) the VESA one, so what is seen on screen can be told apart.
	 */
	static constexpr uint32_t TestDelayDefaultSeconds {120};
	static constexpr uint32_t TestGapSeconds {60};
	bool testEnabled {false};
	bool testStarted {false}; // the test runs once, on the first suitable display
	bool testVesa {false};    // -auxdimtestvesa: also sweep the VESA interface, a minute after the Intel one
	uint32_t testDelaySeconds {TestDelayDefaultSeconds};
	uint32_t testStage {0};
	IOService *testDisplay {nullptr};
	thread_call_t testCall {nullptr};

	void armTest(uint32_t seconds);
	static void testEntry(thread_call_param_t param0, thread_call_param_t param1);

	/**
	 *  Brightness control (-auxdimctl, or auxdimnits=N for a fixed level at boot). Needs the probe to have found a
	 *  usable Intel nits interface; until then requests are only remembered.
	 */
	bool ctlEnabled {false};
	bool ctlAttached {false};
	uint32_t ctlRangeMisses {0}; // brightness requests that carried no usable min/max
	Controller controller;

	/**
	 *  Observed brightness traffic. The hook only counts; a delayed thread call publishes the counters as
	 *  properties (brightness-events, brightness-last), which proves the input path end to end without logs.
	 */
	uint32_t brightnessEvents {0};
	uint32_t lastBrightness {0};
	uint32_t lastLinearBrightness {0};
	thread_call_t statusCall {nullptr};

	static void statusEntry(thread_call_param_t param0, thread_call_param_t param1);
};

#endif /* kern_auxdim_hpp */
