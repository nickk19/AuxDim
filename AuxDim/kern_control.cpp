//
//  kern_control.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <stdarg.h>

#include "kern_control.hpp"
#include "kern_policy.hpp"
#include "kern_probe.hpp"
#include "kern_dpaux.hpp"
#include "kern_backlight.hpp"

namespace {

	/**
	 *  Property only, no log line: this runs on every slider step.
	 */
	void setStatus(const char *key, const char *format, ...) __printflike(2, 3);

	void setStatus(const char *key, const char *format, ...) {
		char line[240];
		va_list va;
		va_start(va, format);
		vsnprintf(line, sizeof(line), format, va);
		va_end(va);
		if (ADDPR(selfInstance))
			ADDPR(selfInstance)->setProperty(key, line);
	}
}

bool Controller::init(uint32_t forced, uint32_t floorLevel) {
	forcedNits = forced;
	if (floorLevel > 0)
		floorNits = floorLevel;

	lock = IOSimpleLockAlloc();
	applyCall = thread_call_allocate(&Controller::applyEntry, this);
	verifyCall = thread_call_allocate(&Controller::verifyEntry, this);
	if (!lock || !applyCall || !verifyCall) {
		deinit();
		return false;
	}
	return true;
}

void Controller::deinit() {
	ready = false;

	if (applyCall) {
		while (!thread_call_free(applyCall))
			thread_call_cancel(applyCall);
		applyCall = nullptr;
	}

	if (verifyCall) {
		while (!thread_call_free(verifyCall))
			thread_call_cancel(verifyCall);
		verifyCall = nullptr;
	}

	if (lock) {
		IOSimpleLockFree(lock);
		lock = nullptr;
	}

	if (target) {
		target->release();
		target = nullptr;
	}
}

void Controller::attach(IOService *display, uint32_t maxLevel) {
	if (ready || !display || !lock)
		return;

	maxNits = maxLevel;
	// Never let the floor reach the top of the range, whatever the boot argument says.
	if (floorNits >= maxNits)
		floorNits = maxNits / 4;
	if (forcedNits) {
		if (forcedNits < floorNits)
			forcedNits = floorNits;
		if (forcedNits > maxNits)
			forcedNits = maxNits;
		forcedPending = true;
	}

	display->retain();
	target = display;

	bool backlog = false;
	IOSimpleLockLock(lock);
	backlog = source == target && (latest[0].valid || latest[1].valid);
	if (forcedPending) {
		// An explicit boot level goes first; whatever macOS asks for after this point replaces it.
		latest[0].valid = false;
		latest[1].valid = false;
	}
	IOSimpleLockUnlock(lock);

	__sync_synchronize();
	ready = true;

	AuxDimProbe::publishf("ctl-state", "ready: Intel nits interface, %u...%u nits, forced level %u, brightness request already waiting: %s",
						 floorNits, maxNits, forcedNits, (backlog && !forcedPending) ? "yes" : "no");

	if (backlog || forcedPending)
		schedule(50);
}

void Controller::request(IOService *display, bool linear, uint32_t value, uint32_t min, uint32_t max) {
	if (!lock || !display)
		return;
	if (ready && display != target)
		return; // another display (an external monitor): not ours

	IOSimpleLockLock(lock);
	if (linear) {
		linearSeen = true;
	} else if (linearSeen) {
		IOSimpleLockUnlock(lock);
		return;
	}
	source = display;
	Pending &entry = latest[linear ? 1 : 0];
	entry.valid = true;
	entry.value = value;
	entry.min = min;
	entry.max = max;
	IOSimpleLockUnlock(lock);

	if (ready)
		schedule(ThrottleMs);
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

void Controller::apply() {
	// Clear the flag before looking at the data: anything arriving from now on schedules a new pass.
	applyScheduled = 0;
	__sync_synchronize();

	Pending chosen;
	bool linear = false;
	bool have = false;
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
	IOSimpleLockUnlock(lock);

	uint32_t nits = 0;
	bool fromForced = false;
	if (have && from == target) {
		if (chosen.max <= chosen.min) {
			setStatus("ctl-last", "ignored: parameter range %u...%u is empty", chosen.min, chosen.max);
			return;
		}
		nits = Policy::nitsFor(chosen.value, chosen.min, chosen.max, linear, floorNits, maxNits);
		forcedPending = false; // a real request replaces the boot-time level
	} else if (forcedPending) {
		nits = forcedNits;
		fromForced = true;
	} else {
		return;
	}

	IOI2CInterface *aux = DPAux::copyNativeAux(target);
	IOReturn writeRet = kIOReturnNotAttached;
	if (aux) {
		writeRet = Backlight::writeIntel(aux, static_cast<uint16_t>(nits));
		aux->release();
	}

	writes++;
	if (writeRet == kIOReturnSuccess) {
		retries = 0;
		lastNits = nits;
		if (fromForced)
			forcedPending = false;

		// The panel needs a moment to ramp, an immediate read shows the old level. Check once things have been quiet:
		// every new write pushes this call back.
		uint64_t deadline = 0;
		clock_interval_to_deadline(VerifyMs, kMillisecondScale, &deadline);
		thread_call_enter_delayed(verifyCall, deadline);
	} else {
		failures++;
		// Most likely the panel is asleep or just woken: keep the request and try again shortly.
		if (retries < MaxRetries) {
			retries++;
			if (have) {
				IOSimpleLockLock(lock);
				Pending &entry = latest[linear ? 1 : 0];
				if (!entry.valid) { // unless a newer one arrived meanwhile
					entry = chosen;
					entry.valid = true;
				}
				IOSimpleLockUnlock(lock);
			}
			schedule(RetryMs);
		}
	}

	// Publish only when the slider has settled, not on every step of a drag.
	if (!applyScheduled) {
		if (fromForced)
			setStatus("ctl-last", "uptime %us: boot level %u nits, ret 0x%x, writes %u, failures %u",
					  AuxDimProbe::uptimeSeconds(), nits, writeRet, writes, failures);
		else
			setStatus("ctl-last", "uptime %us: %s %u (range %u...%u) -> %u nits, ret 0x%x, writes %u, failures %u",
					  AuxDimProbe::uptimeSeconds(), linear ? "linear-brightness" : "brightness", chosen.value,
					  chosen.min, chosen.max, nits, writeRet, writes, failures);
	}
}

void Controller::verifyEntry(thread_call_param_t param0, thread_call_param_t) {
	static_cast<Controller *>(param0)->verify();
}

void Controller::verify() {
	if (!ready || !target)
		return;

	const uint32_t expected = lastNits;
	IOI2CInterface *aux = DPAux::copyNativeAux(target);
	if (!aux)
		return;

	uint16_t level = 0;
	IOReturn ret = Backlight::readIntel(aux, level);
	aux->release();

	// This panel reports one nit below what it was given, so one either way is as expected.
	uint32_t difference = level > expected ? level - expected : expected - level;
	setStatus("ctl-verify", "uptime %us: wrote %u nits, the panel reports %u (ret 0x%x): %s",
			  AuxDimProbe::uptimeSeconds(), expected, level, ret,
			  ret != kIOReturnSuccess ? "could not be read" : (difference <= 1 ? "as expected" : "differs"));
}
