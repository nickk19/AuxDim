//
//  kern_control.hpp
//  AuxDim
//
//  Phase 1: applies the brightness macOS asks for to the panel, through the backend it was given (kern_backend.hpp).
//
//  Input comes from the IODisplay::doIntegerSet hook, which runs with the framebuffer lock held, so
//  request() only records the value. A thread call does the AUX I/O: it is throttled (at most one pass
//  every ThrottleMs, always using the newest value, so dragging the slider is smooth and the AUX channel
//  is not flooded) and retried a few times when the panel is not ready (asleep, just woken). When the display comes back
//  on after being off, the level is written again. If the IODisplay the controller drives is terminated (the framebuffer
//  replaced it), the controller moves to its replacement once the probe has vetted it.
//
//  Enabled by a backend argument (-auxdimintel, -auxdimamd, see kern_backend.hpp).
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_control_hpp
#define kern_control_hpp

#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/i2c/IOI2CInterface.h>
#include <kern/thread_call.h>

#include "kern_backend.hpp"

class Controller {
public:
	/**
	 *  @param backend     how to talk to the panel, see kern_backend.hpp
	 *  @param forcedNits  if not zero, applied once as soon as the controller is up (boot argument auxdimnits)
	 *  @param floorNits   lowest luminance ever sent (boot argument auxdimminnits)
	 *
	 *  @return false when the thread call or the lock could not be allocated
	 */
	bool init(const Backend::Ops *backend, uint32_t forcedNits, uint32_t floorNits);

	/**
	 *  Offer a display whose interface the probe found usable for the backend. Runs on the probe thread.
	 *
	 *  The next pass of the apply thread takes it over and applies the request macOS made before this point (typically the
	 *  saved brightness at boot). While another display is being driven, the offer is kept as the spare that replaces it
	 *  if it goes away.
	 *
	 *  @param maxNits luminance at the top of the range, from Policy::maxNitsFor; refused unless plausible
	 *
	 *  @return false when refused
	 */
	bool attach(IOService *display, uint32_t maxNits);

	/**
	 *  Whether a live display is being driven.
	 */
	bool attached();

	/**
	 *  Brightness requests from displays the controller does not drive (an external monitor), for the status lines.
	 */
	uint32_t foreignRequests() const { return foreign; }

	/**
	 *  Record a brightness request. Called from the hook: does not block, does no I/O.
	 *
	 *  Tahoe's slider only moves `linear-brightness`; `brightness` keeps the value it had at start and can be sent again
	 *  later (by the power handling of a display without a `dsyp` parameter). After the first linear request, plain
	 *  brightness is therefore ignored.
	 *
	 *  @param min  lowest value of the display parameter that carried the request
	 *  @param max  highest value
	 */
	void request(IOService *display, bool linear, uint32_t value, uint32_t min, uint32_t max);

	/**
	 *  Record a display power change (the `dsyp` parameter: off, dimmed, on). Called from the hook: does not block, does no I/O.
	 *  When the display is on again after having been off, the level it should have is written again, after the backend has
	 *  checked that the panel still takes its brightness from AUX (Backend::Ops::activate); the result is published as ctl-wake.
	 */
	void powerState(IOService *display, uint32_t state);

private:
	static constexpr uint32_t ThrottleMs   {30};
	static constexpr uint32_t RetryBaseMs  {1000}; // a failed write is retried after 1, 2, 4, 8 and 16 s
	static constexpr uint32_t MaxRetries   {5};
	static constexpr uint32_t VerifyMs     {600};
	static constexpr uint32_t WakeDelayMs  {200};
	static constexpr uint32_t DefaultFloorNits {0}; // the lowest slider position darkens the panel completely, as on a Mac

	struct Pending {
		bool valid {false};
		uint32_t value {0};
		uint32_t min {0};
		uint32_t max {0};
	};

	const Backend::Ops *backend {nullptr};

	IOSimpleLock *lock {nullptr};
	thread_call_t applyCall {nullptr};
	thread_call_t retryCall {nullptr};  // its own call, so a new request never waits behind a retry that is seconds away
	thread_call_t verifyCall {nullptr};

	// The AUX interface of the target, kept between writes instead of walking the registry on every slider step.
	// auxLock serializes the AUX I/O of apply() and verify() and guards aux. It is a mutex because the I/O blocks;
	// the hook never takes it (lock order: auxLock, then the framebuffer lock inside startIO).
	IOLock *auxLock {nullptr};
	IOI2CInterface *aux {nullptr}; // retained

	// Guarded by lock
	Pending latest[2] {};        // [0] brightness, [1] linear-brightness, both from `source`; each pass consumes them
	IOService *source {nullptr}; // the display the requests in `latest` belong to; compared, never dereferenced
	bool sourceBacklight {false};
	bool linearSeen {false};
	bool wasOff {false};         // the display went off and has not been on since
	bool wakeRequested {false};  // it is on again: the next pass restores the level
	IOService *offered {nullptr};  // retained: handed over by attach(), taken over by the next pass
	uint32_t offeredMaxNits {0};
	IOService *spare {nullptr};    // retained: a usable display offered while another one was driven
	uint32_t spareMaxNits {0};
	volatile uint32_t foreign {0};

	// Written by the apply thread only, under lock (target also under auxLock, which currentAux() reads it with)
	IOService *target {nullptr}; // retained
	volatile bool ready {false};

	// Settings
	uint32_t floorRequested {0};
	uint32_t forcedNits {0};

	// Used by the apply thread only
	volatile uint32_t applyScheduled {0};
	uint32_t maxNits {0};
	uint32_t floorNits {DefaultFloorNits};
	bool forcedPending {false};
	bool attachedBefore {false};
	uint32_t detaches {0};
	uint32_t desiredNits {0};        // the level the panel should have: newest request, or the boot level
	bool desiredPending {false};     // desiredNits has not reached the panel yet
	uint32_t attempts {0};           // failed writes of desiredNits; every new request starts again from 0
	uint32_t writes {0};
	uint32_t failures {0};
	volatile uint32_t lastNits {0};  // last level written successfully, what verify() expects the panel to report
	bool haveLast {false};           // lastNits is valid (0 is a level like any other)
	bool activated {false};          // the backend has confirmed its interface on the target (activate)
	uint64_t ioLastUs {0};           // AUX time of the writes (activation included): the framebuffer lock is held that long
	uint64_t ioMaxUs {0};
	uint64_t ioTotalUs {0};
	uint32_t ioCount {0};
	bool switchedOn {false};         // an activation switched the interface on, not reported yet (the write may have failed)
	uint8_t switchedFrom {0};        // the mode register before that
	bool wakeCheck {false};          // activate again before the next write, then report it as ctl-wake
	uint32_t wakes {0};

	// What desiredNits came from, for the status line
	bool statusForced {false};
	bool statusLinear {false};
	Pending statusRequest {};

	/**
	 *  The AUX interface to use, looked up again when there is none or the one kept went away. Call with auxLock held.
	 */
	IOI2CInterface *currentAux();

	/**
	 *  Forget the interface kept, after a failed transfer: the next one looks it up again. Call with auxLock held.
	 */
	void dropAux();

	/**
	 *  Start driving a display (the reference passes to target), or stop driving the one that went away. Apply thread only.
	 */
	void takeOver(IOService *display, uint32_t maxLevel);
	void detach();

	void schedule(uint32_t delayMs);
	void apply();
	void verify();
	static void applyEntry(thread_call_param_t param0, thread_call_param_t param1);
	static void retryEntry(thread_call_param_t param0, thread_call_param_t param1);
	static void verifyEntry(thread_call_param_t param0, thread_call_param_t param1);
};

#endif /* kern_control_hpp */
