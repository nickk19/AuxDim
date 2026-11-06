//
//  kern_control.hpp
//  AuxDim
//
//  Phase 1: applies the brightness macOS asks for to the panel, through the Intel nits interface.
//
//  Input comes from the IODisplay::doIntegerSet hook, which runs with the framebuffer lock held, so
//  request() only records the value. A thread call does the AUX I/O: it is throttled (at most one pass
//  every ThrottleMs, always using the newest value, so dragging the slider is smooth and the AUX channel
//  is not flooded) and retried a few times when the panel is not ready (asleep, just woken).
//
//  Enabled with -auxdimctl (or auxdimnits=N). Never active together with -auxdimtest.
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_control_hpp
#define kern_control_hpp

#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>

class Controller {
public:
	/**
	 *  @param forcedNits  if not zero, applied once as soon as the controller is up (boot argument auxdimnits)
	 *  @param floorNits   lowest luminance ever sent (boot argument auxdimminnits)
	 *
	 *  @return false when the thread call or the lock could not be allocated
	 */
	bool init(uint32_t forcedNits, uint32_t floorNits);
	void deinit();

	/**
	 *  Bring the controller up on a display whose Intel nits interface the probe found usable.
	 *  Applies the request macOS made before this point (typically the saved brightness at boot).
	 *  Runs on the probe thread.
	 */
	void attach(IOService *display, uint32_t maxNits);

	/**
	 *  Record a brightness request. Called from the hook: does not block, does no I/O.
	 *
	 *  Tahoe's slider only moves `linear-brightness`; `brightness` keeps the value it had at start and can be
	 *  sent again later (power handling). After the first linear request, plain brightness is therefore ignored.
	 *
	 *  @param min  lowest value of the display parameter that carried the request
	 *  @param max  highest value
	 */
	void request(IOService *display, bool linear, uint32_t value, uint32_t min, uint32_t max);

private:
	static constexpr uint32_t ThrottleMs   {30};
	static constexpr uint32_t RetryMs      {1000};
	static constexpr uint32_t MaxRetries   {5};
	static constexpr uint32_t VerifyMs     {600};

	struct Pending {
		bool valid {false};
		uint32_t value {0};
		uint32_t min {0};
		uint32_t max {0};
	};

	IOSimpleLock *lock {nullptr};
	thread_call_t applyCall {nullptr};
	thread_call_t verifyCall {nullptr};

	// Guarded by lock
	Pending latest[2] {};      // [0] brightness, [1] linear-brightness; each pass consumes them
	IOService *source {nullptr};
	bool linearSeen {false};

	// Set once by attach(), before ready
	IOService *target {nullptr}; // retained
	uint32_t maxNits {0};
	uint32_t floorNits {10};
	uint32_t forcedNits {0};
	bool forcedPending {false};
	volatile bool ready {false};

	// Used by the apply thread only
	volatile uint32_t applyScheduled {0};
	uint32_t retries {0};
	uint32_t writes {0};
	uint32_t failures {0};
	volatile uint32_t lastNits {0};  // last level written successfully, what verify() expects the panel to report

	void schedule(uint32_t delayMs);
	void apply();
	void verify();
	static void applyEntry(thread_call_param_t param0, thread_call_param_t param1);
	static void verifyEntry(thread_call_param_t param0, thread_call_param_t param1);
};

#endif /* kern_control_hpp */
