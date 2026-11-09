//
//  kern_auxdim.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <kern/clock.h>

#include "kern_auxdim.hpp"
#include "kern_probe.hpp"
#include "kern_policy.hpp"

#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>

AuxDimCore *AuxDimCore::callbackAuxDim;

static const char *pathIOGraphics[] { "/System/Library/Extensions/IOGraphicsFamily.kext/IOGraphicsFamily" };

static KernelPatcher::KextInfo kextIOGraphics {
	"com.apple.iokit.IOGraphicsFamily", pathIOGraphics, arrsize(pathIOGraphics),
	{true}, {}, KernelPatcher::KextInfo::Unloaded
};

void AuxDimCore::init() {
	callbackAuxDim = this;

	probeEnabled = checkKernelArgument("-auxdimprobe");

	// Brightness control is opt-in. A fixed boot level implies it.
	uint32_t forcedNits = 0, floorNits = 0;
	lilu_get_boot_args("auxdimnits", &forcedNits, sizeof(forcedNits));
	lilu_get_boot_args("auxdimminnits", &floorNits, sizeof(floorNits));
	lilu_get_boot_args("auxdimmaxnits", &ctlMaxNitsOverride, sizeof(ctlMaxNitsOverride));
	if (ctlMaxNitsOverride && !Policy::plausibleMaxNits(ctlMaxNitsOverride)) {
		SYSLOG("auxdim", "auxdimmaxnits=%u is outside %u...%u, ignored", ctlMaxNitsOverride, Policy::MinMaxNits, Policy::MaxMaxNits);
		ctlMaxNitsOverride = 0;
	}
	ctlEnabled = checkKernelArgument("-auxdimctl") || forcedNits > 0;
	if (ctlEnabled) {
		probeEnabled = true;
		if (!controller.init(forcedNits, floorNits)) {
			SYSLOG("auxdim", "failed to set up the brightness controller, control disabled");
			ctlEnabled = false;
		}
	}

	if (probeEnabled) {
		// ONCE: never two instances of the same call in parallel, see Controller::init.
		probeCall = thread_call_allocate_with_options(&AuxDimCore::probeEntry, this, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
		statusCall = thread_call_allocate_with_options(&AuxDimCore::statusEntry, this, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
		if (!probeCall) {
			SYSLOG("auxdim", "failed to allocate the probe thread call, probe disabled");
			probeEnabled = false;
			ctlEnabled = false;
		}
	}

	// The hook serves the probe and the controller, and in the Debug build the request log of -auxdimdbg. Without any of
	// them IOGraphicsFamily is left untouched.
	bool watch = probeEnabled || ctlEnabled;
#ifdef DEBUG
	watch = watch || ADDPR(debugEnabled);
#endif
	if (!watch) {
		SYSLOG("auxdim", "neither -auxdimctl nor -auxdimprobe given, nothing to do");
		return;
	}

	// Not onKextLoadForce: that one panics when Lilu cannot take the request. A diagnostic plugin stays inert instead.
	auto error = lilu.onKextLoad(&kextIOGraphics, 1,
	[](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		static_cast<AuxDimCore *>(user)->processKext(patcher, index, address, size);
	}, this);
	if (error != LiluAPI::Error::NoError)
		SYSLOG("auxdim", "cannot watch for IOGraphicsFamily (Lilu error %d), plugin is inert", static_cast<int>(error));
}

void AuxDimCore::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (kextIOGraphics.loadIndex != index)
		return;

	KernelPatcher::RouteRequest request(
		"__ZN9IODisplay12doIntegerSetEP12OSDictionaryPK8OSSymbolj", wrapDoIntegerSet, orgDoIntegerSet);

	if (!patcher.routeMultiple(index, &request, 1, address, size)) {
		SYSLOG("auxdim", "failed to route IODisplay::doIntegerSet, plugin is inert");
		patcher.clearError();
		return;
	}

	DBGLOG("auxdim", "IODisplay::doIntegerSet routed, observing display parameters (probe %s)",
		   probeEnabled ? "enabled" : "disabled");
}

bool AuxDimCore::wrapDoIntegerSet(void *that, OSDictionary *params, const OSSymbol *name, UInt32 value) {
	// Always forward first: this phase never changes what the stack does.
	bool handled = FunctionCast(wrapDoIntegerSet, callbackAuxDim->orgDoIntegerSet)(that, params, name, value);

	if (that && name)
		callbackAuxDim->handleParameter(reinterpret_cast<IOService *>(that), params, name, value, handled);

	return handled;
}

void AuxDimCore::handleParameter(IOService *display, OSDictionary *params, const OSSymbol *name, UInt32 value, bool handled) {
	bool linear = name->isEqualTo(kIODisplayLinearBrightnessKey);
	if (linear || name->isEqualTo(kIODisplayBrightnessKey)) {
		brightnessEvents++;
		if (linear)
			lastLinearBrightness = value;
		else
			lastBrightness = value;

		scheduleStatus();
		if (ctlEnabled && params) {
			// The range of this very parameter, so the mapping needs no assumption about macOS's scale.
			// IODisplay::setProperties() hands over the entry of the parameter itself ({min, max, value}); other
			// callers (power and fade handling) hand over the whole parameter dictionary, the entry is inside it.
			OSDictionary *entry = params;
			if (!OSDynamicCast(OSNumber, entry->getObject(kIODisplayMaxValueKey)))
				entry = OSDynamicCast(OSDictionary, params->getObject(name));

			auto *min = entry ? OSDynamicCast(OSNumber, entry->getObject(kIODisplayMinValueKey)) : nullptr;
			auto *max = entry ? OSDynamicCast(OSNumber, entry->getObject(kIODisplayMaxValueKey)) : nullptr;
			if (min && max) {
				controller.request(display, linear, value, min->unsigned32BitValue(), max->unsigned32BitValue());
			} else {
				ctlRangeMisses++; // published by statusEntry: a request dropped for want of a range must be visible
			}
		}

		DBGLOG("auxdim", "%s on %s: %s = %u (accepted by the stack: %s, events so far: %u)",
			   display->getName(), display->getMetaClass()->getClassName(),
			   name->getCStringNoCopy(), value, handled ? "yes" : "no", brightnessEvents);
	} else if (name->isEqualTo(kIODisplayPowerStateKey)) {
		// Display power (off, dimmed, on). Displays that have this parameter get it instead of brightness 0 when they sleep.
		powerEvents++;
		lastPower = value;
		scheduleStatus();
		if (ctlEnabled)
			controller.powerState(display, value);

		DBGLOG("auxdim", "%s on %s: %s = %u (accepted by the stack: %s)", display->getName(),
			   display->getMetaClass()->getClassName(), name->getCStringNoCopy(), value, handled ? "yes" : "no");
	}

	// Any parameter is enough to learn that this display exists: the first one triggers the probe,
	// which is useful even when no brightness request ever arrives (that is a finding on its own).
	if (probeEnabled)
		scheduleProbe(display);
}

void AuxDimCore::scheduleStatus() {
	// Publish the counters from a thread call, never from the hook (framebuffer lock held).
	if (statusCall) {
		uint64_t deadline = 0;
		clock_interval_to_deadline(250, kMillisecondScale, &deadline);
		thread_call_enter_delayed(statusCall, deadline);
	}
}

void AuxDimCore::scheduleProbe(IOService *display) {
	// Lock free fast path: this runs for every display parameter, with the framebuffer lock held. Keyed by registry ID, not
	// by pointer: a display that replaces a terminated one may get the same address.
	const uint64_t key = display->getRegistryEntryID();
	for (size_t i = 0; i < probedCount && i < MaxProbedDisplays; i++) {
		if (probed[i] == key)
			return;
	}

	if (!__sync_bool_compare_and_swap(&probeBusy, 0, 1))
		return; // another probe is in flight, this display will be picked up by its next parameter

	bool already = false;
	for (size_t i = 0; i < probedCount && i < MaxProbedDisplays; i++)
		already |= (probed[i] == key);

	if (already || probedCount >= MaxProbedDisplays) {
		probeBusy = 0;
		return;
	}

	probed[probedCount++] = key;
	display->retain();
	probeDisplay = display;
	ctlAttempts = 0;

	uint64_t deadline = 0;
	clock_interval_to_deadline(ProbeDelayMs, kMillisecondScale, &deadline);
	thread_call_enter_delayed(probeCall, deadline);
}

void AuxDimCore::probeEntry(thread_call_param_t param0, thread_call_param_t) {
	auto *self = static_cast<AuxDimCore *>(param0);

	IOService *display = self->probeDisplay;
	self->probeDisplay = nullptr;

	bool retrying = false;
	if (display) {
		AuxDimProbe::Result result;
		AuxDimProbe::run(display, &result);

		// Offer every display with a usable Intel nits interface to the controller: the first one is driven, a later one
		// replaces it if it goes away. The status lines are about the display being looked for, so they stay quiet once
		// one is driven.
		uint32_t maxNits = Policy::maxNitsFor(result.edidMaxNits, self->ctlMaxNitsOverride);
		if (self->ctlEnabled && result.intelUsable && maxNits) {
			self->controller.attach(display, maxNits);
		} else if (self->ctlEnabled && !self->controller.attached()) {
			if (result.intelUsable) {
				AuxDimProbe::publishf("ctl-state", "inert on this display: the EDID gives no usable luminance range (frame-average "
									 "%u nits, expected %u...%u); set auxdimmaxnits=N to the panel's full brightness to enable control",
									 result.edidMaxNits, Policy::MinMaxNits, Policy::MaxMaxNits);
			} else if (result.retryable && ++self->ctlAttempts < RetryMaxCount) {
				// Keep the display and the busy flag: nobody else is probed meanwhile, this one is tried again.
				uint32_t delayMs = self->ctlAttempts < RetryFastCount ? RetryFastMs : RetrySlowMs;
				AuxDimProbe::publishf("ctl-state", "waiting: %s (error 0x%x), attempt %u of %u, next one in %u s",
									 result.reason, result.error, self->ctlAttempts, RetryMaxCount, delayMs / 1000);
				self->probeDisplay = display;
				retrying = true;

				uint64_t deadline = 0;
				clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
				thread_call_enter_delayed(self->probeCall, deadline);
			} else if (result.retryable) {
				AuxDimProbe::publishf("ctl-state", "inert on this display: %s (error 0x%x), gave up after %u attempts",
									 result.reason, result.error, self->ctlAttempts);
			} else {
				AuxDimProbe::publishf("ctl-state", "inert on this display: %s", result.reason);
			}
		}

		if (!retrying)
			display->release();
	}

	if (!retrying)
		self->probeBusy = 0;
}

void AuxDimCore::statusEntry(thread_call_param_t param0, thread_call_param_t) {
	// Counters, refreshed after every slider move: properties only, no log lines.
	auto *self = static_cast<AuxDimCore *>(param0);
	AuxDimProbe::statusf("brightness-events", "%u", self->brightnessEvents);
	AuxDimProbe::statusf("brightness-last", "brightness=%u linear-brightness=%u uptime=%us",
						 self->lastBrightness, self->lastLinearBrightness, AuxDimProbe::uptimeSeconds());
	if (self->powerEvents)
		AuxDimProbe::statusf("power-last", "dsyp=%u events=%u uptime=%us", self->lastPower, self->powerEvents,
							 AuxDimProbe::uptimeSeconds());
	if (self->ctlEnabled) {
		AuxDimProbe::statusf("ctl-range-misses", "%u", self->ctlRangeMisses);
		AuxDimProbe::statusf("ctl-foreign", "%u", self->controller.foreignRequests());
	}
}
