//
//  kern_control.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/kern_api.hpp>

#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <kern/clock.h>
#include <string.h>

#include "kern_control.hpp"
#include "kern_policy.hpp"
#include "kern_probe.hpp"
#include "kern_dpaux.hpp"

namespace {

	/**
	 *  The panel's IODisplay is an IOBacklightDisplay (AppleBacklightDisplay with SSDT-PNLF); an external monitor is not.
	 *  By class name, so that nothing from IOGraphicsFamily is needed at link time. No allocation, no lock.
	 */
	bool isBacklightDisplay(const IOService *display) {
		static const char name[] = "IOBacklightDisplay";
		for (const OSMetaClass *meta = display->getMetaClass(); meta; meta = meta->getSuperClass()) {
			const char *className = meta->getClassName();
			if (className && strncmp(className, name, sizeof(name)) == 0)
				return true;
		}
		return false;
	}
}

bool Controller::init(const Backend::Ops *ops, uint32_t forced, uint32_t floorLevel) {
	if (!ops)
		return false;
	backend = ops;
	forcedNits = forced;
	floorRequested = floorLevel;

	lock = IOSimpleLockAlloc();
	auxLock = IOLockAlloc();
	// ONCE: a call armed again while it runs is queued behind the running instance instead of starting a second one
	// in parallel (xnu thread_call.c), so two writes can never race each other to the panel.
	applyCall = thread_call_allocate_with_options(&Controller::applyEntry, this, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
	retryCall = thread_call_allocate_with_options(&Controller::retryEntry, this, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
	verifyCall = thread_call_allocate_with_options(&Controller::verifyEntry, this, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
	if (!lock || !auxLock || !applyCall || !retryCall || !verifyCall) {
		// Nothing was ever scheduled, so the calls can be freed right away. There is no other teardown: the
		// plugin is never unloaded once started.
		if (applyCall)
			thread_call_free(applyCall);
		if (retryCall)
			thread_call_free(retryCall);
		if (verifyCall)
			thread_call_free(verifyCall);
		if (lock)
			IOSimpleLockFree(lock);
		if (auxLock)
			IOLockFree(auxLock);
		applyCall = retryCall = verifyCall = nullptr;
		lock = nullptr;
		auxLock = nullptr;
		return false;
	}
	return true;
}

bool Controller::attach(IOService *display, uint32_t maxLevel) {
	// The range keeps every written level far below 0xFFFF, the size of the register.
	if (!display || !lock || !Policy::plausibleMaxNits(maxLevel))
		return false;

	IOService *drop = nullptr;
	display->retain();
	IOSimpleLockLock(lock);
	const bool now = !target || target->isInactive();
	if (now) {
		drop = offered;
		offered = display;
		offeredMaxNits = maxLevel;
	} else {
		drop = spare;
		spare = display;
		spareMaxNits = maxLevel;
	}
	IOSimpleLockUnlock(lock);

	if (drop)
		drop->release();
	if (now)
		schedule(0);
	return true;
}

bool Controller::attached() {
	if (!lock)
		return false;
	IOSimpleLockLock(lock);
	bool live = target && !target->isInactive();
	IOSimpleLockUnlock(lock);
	return live;
}

void Controller::takeOver(IOService *display, uint32_t maxLevel) {
	activated = false;
	switchedOn = false;
	maxNits = maxLevel;
	// Never let the floor reach the top of the range, or zero, whatever the boot argument says.
	floorNits = Policy::floorNitsFor(floorRequested ? floorRequested : DefaultFloorNits, maxNits);
	// The boot level is for the first display only.
	const bool forced = forcedNits && !attachedBefore;
	if (forced) {
		if (forcedNits < floorNits)
			forcedNits = floorNits;
		if (forcedNits > maxNits)
			forcedNits = maxNits;
		forcedPending = true;
	}
	attachedBefore = true;

	bool backlog = false;
	IOLockLock(auxLock);
	IOSimpleLockLock(lock);
	target = display;
	backlog = source == target && (latest[0].valid || latest[1].valid);
	if (forced) {
		// An explicit boot level goes first; whatever macOS asks for after this point replaces it.
		latest[0].valid = false;
		latest[1].valid = false;
	}
	wasOff = false;
	wakeRequested = false;
	IOSimpleLockUnlock(lock);
	IOLockUnlock(auxLock);

	__sync_synchronize();
	ready = true;

	AuxDimProbe::publishf("ctl-state", "ready: %s, %u...%u nits, forced level %u, brightness request already waiting: %s%s",
						 backend->name, floorNits, maxNits, forced ? forcedNits : 0, (backlog && !forced) ? "yes" : "no",
						 detaches ? ", on the display that replaced the previous one" : "");
}

void Controller::detach() {
	IOLockLock(auxLock);
	dropAux();
	IOSimpleLockLock(lock);
	IOService *old = target;
	target = nullptr;
	ready = false;
	IOSimpleLockUnlock(lock);
	IOLockUnlock(auxLock);

	// Whatever was still to be written was meant for the display that went away.
	desiredPending = false;
	forcedPending = false;
	attempts = 0;
	activated = false;
	switchedOn = false;
	wakeCheck = false;
	detaches++;
	AuxDimProbe::publishf("ctl-state", "detached: the display went away (%u so far), waiting for its replacement", detaches);

	if (old)
		old->release();
}

void Controller::request(IOService *display, bool linear, uint32_t value, uint32_t min, uint32_t max) {
	if (!lock || !display)
		return;

	IOSimpleLockLock(lock);
	const bool targetGone = target && target->isInactive();
	if (target && display != target && !targetGone) {
		// Another display (an external monitor): not ours.
		foreign++;
		IOSimpleLockUnlock(lock);
		return;
	}
	if (display != source) {
		// Requests are kept for one display at a time: before attach (and after the driven display went away) the last one
		// that asked. A display with a backlight, the panel, is never displaced by one without.
		const bool backlight = isBacklightDisplay(display);
		if (source && sourceBacklight && !backlight) {
			foreign++;
			IOSimpleLockUnlock(lock);
			return;
		}
		source = display;
		sourceBacklight = backlight;
		linearSeen = false;
		latest[0].valid = false;
		latest[1].valid = false;
	}
	if (linear) {
		linearSeen = true;
	} else if (linearSeen) {
		IOSimpleLockUnlock(lock);
		return;
	}
	Pending &entry = latest[linear ? 1 : 0];
	entry.valid = true;
	entry.value = value;
	entry.min = min;
	entry.max = max;
	const bool ours = display == target;
	IOSimpleLockUnlock(lock);

	if (ours && ready)
		schedule(ThrottleMs);
	else if (targetGone && ready)
		schedule(0); // let the apply thread notice: it moves to the spare, if the probe has offered one
}

void Controller::powerState(IOService *display, uint32_t state) {
	if (!lock)
		return;

	bool wake = false;
	IOSimpleLockLock(lock);
	if (display == target) {
		if (state == kIODisplayPowerStateOff) {
			wasOff = true;
		} else if (state >= kIODisplayPowerStateOn && wasOff) {
			// Only a real wake: dimming before sleep and back (MinUsable, On) leaves the panel alone.
			wasOff = false;
			wakeRequested = true;
			wake = true;
		}
	}
	IOSimpleLockUnlock(lock);

	if (wake && ready)
		schedule(WakeDelayMs);
}

IOI2CInterface *Controller::currentAux() {
	if (aux && aux->isInactive())
		dropAux();
	if (!aux && target && !target->isInactive())
		aux = DPAux::copyNativeAux(target);
	return aux;
}

void Controller::dropAux() {
	if (aux) {
		aux->release();
		aux = nullptr;
	}
}

void Controller::schedule(uint32_t delayMs) {
	// One pending pass at a time: later requests simply update `latest`, the pass reads the newest.
	if (!__sync_bool_compare_and_swap(&applyScheduled, 0, 1))
		return;

	uint64_t deadline = 0;
	clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
	thread_call_enter_delayed(applyCall, deadline);
}

void Controller::applyEntry(thread_call_param_t param0, thread_call_param_t) {
	static_cast<Controller *>(param0)->apply();
}

void Controller::retryEntry(thread_call_param_t param0, thread_call_param_t) {
	// Only arm a pass: if one is already pending (a request came in meanwhile) it does the retry too.
	static_cast<Controller *>(param0)->schedule(0);
}

void Controller::apply() {
	// Clear the flag before looking at the data: anything arriving from now on schedules a new pass.
	applyScheduled = 0;
	__sync_synchronize();

	// Which display to drive: a new one from the probe, the spare when the driven one went away.
	IOService *newTarget = nullptr;
	uint32_t newMaxNits = 0;
	IOService *staleSpare = nullptr;
	IOSimpleLockLock(lock);
	const bool targetGone = target && target->isInactive();
	newTarget = offered;
	newMaxNits = offeredMaxNits;
	offered = nullptr;
	if (spare && spare->isInactive()) {
		staleSpare = spare;
		spare = nullptr;
	}
	if (targetGone && !newTarget && spare) {
		newTarget = spare;
		newMaxNits = spareMaxNits;
		spare = nullptr;
	}
	IOSimpleLockUnlock(lock);

	if (staleSpare)
		staleSpare->release();
	if (target && (targetGone || newTarget))
		detach();
	if (newTarget)
		takeOver(newTarget, newMaxNits);
	if (!ready)
		return;

	Pending chosen;
	bool linear = false;
	bool have = false;
	bool wake = false;
	IOService *from = nullptr;

	IOSimpleLockLock(lock);
	from = source;
	if (latest[1].valid) {          // linear-brightness wins when both arrived together
		chosen = latest[1];
		linear = true;
		have = true;
	} else if (latest[0].valid) {
		chosen = latest[0];
		have = true;
	}
	latest[0].valid = false;
	latest[1].valid = false;
	wake = wakeRequested;
	wakeRequested = false;
	IOSimpleLockUnlock(lock);

	if (have && from == target && chosen.max <= chosen.min) {
		AuxDimProbe::statusf("ctl-last", "ignored: parameter range %u...%u is empty", chosen.min, chosen.max);
		have = false;
	}

	if (have && from == target) {
		// A real request replaces the boot level and whatever was still being retried, with a fresh retry budget.
		desiredNits = Policy::nitsFor(chosen.value, chosen.min, chosen.max, linear, floorNits, maxNits);
		desiredPending = true;
		forcedPending = false;
		attempts = 0;
		statusForced = false;
		statusLinear = linear;
		statusRequest = chosen;
	} else if (forcedPending) {
		desiredNits = forcedNits;
		desiredPending = true;
		forcedPending = false;
		attempts = 0;
		statusForced = true;
	}

	if (wake) {
		// The panel kept its level across display sleep in every test so far, but nothing guarantees it (deeper sleep states,
		// other firmware): write it again, with a fresh retry budget, once the backend confirms the panel still listens to AUX.
		wakes++;
		if (desiredPending || lastNits) {
			if (!desiredPending)
				desiredNits = lastNits;
			desiredPending = true;
			attempts = 0;
			wakeCheck = true;
		} else {
			AuxDimProbe::publishf("ctl-wake", "uptime %us: wake %u, no level written yet, nothing to restore",
								 AuxDimProbe::uptimeSeconds(), wakes);
		}
	}

	if (!desiredPending)
		return;

	// The range keeps every level well inside the 16 bit register (Policy::MaxMaxNits).
	const uint32_t nits = desiredNits;
	const bool activating = !activated || wakeCheck;
	IOReturn writeRet = kIOReturnNotAttached;
	uint8_t mode = 0;
	bool switched = false;
	bool on = true;
	IOLockLock(auxLock);
	if (IOI2CInterface *bus = currentAux()) {
		writeRet = activating ? backend->activate(bus, mode, switched, on) : kIOReturnSuccess;
		if (writeRet == kIOReturnSuccess && on)
			writeRet = backend->write(bus, nits);
	}
	// The framebuffer may have replaced its interfaces (a wake that failed, a reconfiguration): look again next time.
	if (writeRet != kIOReturnSuccess)
		dropAux();
	IOLockUnlock(auxLock);
	if (switched && !switchedOn) {
		switchedOn = true;
		switchedFrom = mode;
	}
	if (switchedOn)
		mode = switchedFrom;

	if (!on) {
		// The panel does not take its level from this interface (any more), and this backend may not switch it on: report it
		// and leave the panel alone. The next request checks again.
		desiredPending = false;
		activated = false;
		switchedOn = false;
		if (wakeCheck) {
			wakeCheck = false;
			AuxDimProbe::publishf("ctl-wake", "uptime %us: wake %u, %s = 0x%02x: brightness through AUX is off, %u nits not written",
								 AuxDimProbe::uptimeSeconds(), wakes, backend->modeRegister, mode, nits);
		} else {
			AuxDimProbe::publishf("ctl-activate", "uptime %us: %s = 0x%02x: brightness through AUX is off, %u nits not written",
								 AuxDimProbe::uptimeSeconds(), backend->modeRegister, mode, nits);
		}
		return;
	}

	writes++;
	char retryNote[64] = "";
	if (writeRet == kIOReturnSuccess) {
		desiredPending = false;
		attempts = 0;
		lastNits = nits;
		if (activating && !wakeCheck)
			AuxDimProbe::publishf("ctl-activate", "uptime %us: %s = 0x%02x%s", AuxDimProbe::uptimeSeconds(),
								 backend->modeRegister, mode, switchedOn ? ", switched on" : "");
		activated = true;

		// The panel needs a moment to ramp, an immediate read shows the old level. Check once things have been quiet:
		// every new write pushes this call back.
		uint64_t deadline = 0;
		clock_interval_to_deadline(VerifyMs, kMillisecondScale, &deadline);
		thread_call_enter_delayed(verifyCall, deadline);

		if (wakeCheck) {
			wakeCheck = false;
			AuxDimProbe::publishf("ctl-wake", "uptime %us: wake %u, %s = 0x%02x%s, %u nits written",
								 AuxDimProbe::uptimeSeconds(), wakes, backend->modeRegister, mode,
								 switchedOn ? " (switched on)" : "", nits);
		}
		switchedOn = false;
	} else {
		failures++;
		// Most likely the framebuffer is not powered yet (boot, wake): keep the level and try again, waiting longer
		// each time. A new request replaces the level and starts the count again.
		if (attempts < MaxRetries) {
			uint32_t delayMs = RetryBaseMs << attempts;
			attempts++;
			snprintf(retryNote, sizeof(retryNote), ", retry %u of %u in %u s", attempts, MaxRetries, delayMs / 1000);
			uint64_t deadline = 0;
			clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
			thread_call_enter_delayed(retryCall, deadline);
		} else {
			desiredPending = false;
			snprintf(retryNote, sizeof(retryNote), ", gave up after %u retries", MaxRetries);
			if (wakeCheck) {
				wakeCheck = false;
				AuxDimProbe::publishf("ctl-wake", "uptime %us: wake %u, the panel did not answer (ret 0x%x), gave up after %u retries",
									 AuxDimProbe::uptimeSeconds(), wakes, writeRet, MaxRetries);
			}
		}
	}

	// Publish only when the slider has settled, not on every step of a drag.
	if (!applyScheduled || retryNote[0]) {
		if (statusForced)
			AuxDimProbe::statusf("ctl-last", "uptime %us: boot level %u nits, ret 0x%x, writes %u, failures %u%s",
					  AuxDimProbe::uptimeSeconds(), nits, writeRet, writes, failures, retryNote);
		else
			AuxDimProbe::statusf("ctl-last", "uptime %us: %s %u (range %u...%u) -> %u nits, ret 0x%x, writes %u, failures %u%s",
					  AuxDimProbe::uptimeSeconds(), statusLinear ? "linear-brightness" : "brightness", statusRequest.value,
					  statusRequest.min, statusRequest.max, nits, writeRet, writes, failures, retryNote);
	}
}

void Controller::verifyEntry(thread_call_param_t param0, thread_call_param_t) {
	static_cast<Controller *>(param0)->verify();
}

void Controller::verify() {
	if (!ready || !target)
		return;

	const uint32_t expected = lastNits;
	uint32_t level = 0;
	IOReturn ret = kIOReturnSuccess;
	bool found = false;
	IOLockLock(auxLock);
	if (IOI2CInterface *bus = currentAux()) {
		found = true;
		ret = backend->read(bus, level);
		if (ret != kIOReturnSuccess)
			dropAux();
	}
	IOLockUnlock(auxLock);
	if (!found)
		return;

	// The Intel panel reports one nit below what it was given, so one either way is as expected.
	uint32_t difference = level > expected ? level - expected : expected - level;
	AuxDimProbe::statusf("ctl-verify", "uptime %us: wrote %u nits, the panel reports %u (ret 0x%x): %s",
			  AuxDimProbe::uptimeSeconds(), expected, level, ret,
			  ret != kIOReturnSuccess ? "could not be read" : (difference <= 1 ? "as expected" : "differs"));
}
