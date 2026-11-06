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
#include "kern_selftest.hpp"

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

	// The write test is opt-in and builds on the probe.
	testEnabled = checkKernelArgument("-auxdimtest");
	if (testEnabled) {
		probeEnabled = true;
		testVesa = checkKernelArgument("-auxdimtestvesa");
		lilu_get_boot_args("auxdimtestdelay", &testDelaySeconds, sizeof(testDelaySeconds));
		if (testDelaySeconds < 20)
			testDelaySeconds = 20;
		if (testDelaySeconds > 3600)
			testDelaySeconds = 3600;

		testCall = thread_call_allocate(&AuxDimCore::testEntry, this);
		if (!testCall) {
			SYSLOG("auxdim", "failed to allocate the test thread call, write test disabled");
			testEnabled = false;
		}
	}

	// Brightness control is opt-in too. A fixed boot level implies it; the write test excludes it, the two
	// would fight over the same register.
	uint32_t forcedNits = 0, floorNits = 0;
	lilu_get_boot_args("auxdimnits", &forcedNits, sizeof(forcedNits));
	lilu_get_boot_args("auxdimminnits", &floorNits, sizeof(floorNits));
	ctlEnabled = checkKernelArgument("-auxdimctl") || forcedNits > 0;
	if (ctlEnabled && testEnabled) {
		SYSLOG("auxdim", "-auxdimtest and brightness control exclude each other, control disabled for this boot");
		ctlEnabled = false;
	}
	if (ctlEnabled) {
		probeEnabled = true;
		if (!controller.init(forcedNits, floorNits)) {
			SYSLOG("auxdim", "failed to set up the brightness controller, control disabled");
			ctlEnabled = false;
		}
	}

	if (probeEnabled) {
		probeCall = thread_call_allocate(&AuxDimCore::probeEntry, this);
		statusCall = thread_call_allocate(&AuxDimCore::statusEntry, this);
		if (!probeCall) {
			SYSLOG("auxdim", "failed to allocate the probe thread call, probe disabled");
			probeEnabled = false;
			testEnabled = false;
			ctlEnabled = false;
		}
	}

	lilu.onKextLoadForce(&kextIOGraphics, 1,
	[](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		static_cast<AuxDimCore *>(user)->processKext(patcher, index, address, size);
	}, this);
}

void AuxDimCore::deinit() {
	thread_call_t calls[] = { probeCall, testCall, statusCall };
	for (auto call : calls) {
		if (!call)
			continue;
		while (!thread_call_free(call))
			thread_call_cancel(call);
	}
	probeCall = testCall = statusCall = nullptr;

	controller.deinit();

	if (probeDisplay) {
		probeDisplay->release();
		probeDisplay = nullptr;
	}

	if (testDisplay) {
		testDisplay->release();
		testDisplay = nullptr;
	}
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

		// Publish the counters from a thread call, never from here (framebuffer lock held).
		if (statusCall) {
			uint64_t deadline = 0;
			clock_interval_to_deadline(250, kMillisecondScale, &deadline);
			thread_call_enter_delayed(statusCall, deadline);
		}
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
	}

	// Any parameter is enough to learn that this display exists: the first one triggers the probe,
	// which is useful even when no brightness request ever arrives (that is a finding on its own).
	if (probeEnabled)
		scheduleProbe(display);
}

void AuxDimCore::scheduleProbe(IOService *display) {
	// Lock free fast path: this runs for every display parameter, with the framebuffer lock held.
	const uintptr_t key = reinterpret_cast<uintptr_t>(display);
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
		bool writable = AuxDimProbe::run(display, &result);

		// Bring the brightness controller up on the first display with a usable Intel nits interface.
		if (self->ctlEnabled && !self->ctlAttached) {
			if (result.intelUsable) {
				self->ctlAttached = true;
				self->controller.attach(display, result.maxNits);
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

		// Arm the write test on the first display that has something to test.
		if (writable && self->testEnabled && !self->testStarted) {
			self->testStarted = true;
			display->retain();
			self->testDisplay = display;
			self->testStage = 0;
			AuxDimProbe::publishf("test-armed", "Intel nits sweep in %u s%s; uptime now %us",
								 self->testDelaySeconds,
								 self->testVesa ? ", VESA sweep a minute later" : " (VESA sweep not requested)",
								 AuxDimProbe::uptimeSeconds());
			self->armTest(self->testDelaySeconds);
		}

		if (!retrying)
			display->release();
	}

	if (!retrying)
		self->probeBusy = 0;
}

void AuxDimCore::armTest(uint32_t seconds) {
	uint64_t deadline = 0;
	clock_interval_to_deadline(seconds, kSecondScale, &deadline);
	thread_call_enter_delayed(testCall, deadline);
}

void AuxDimCore::testEntry(thread_call_param_t param0, thread_call_param_t) {
	auto *self = static_cast<AuxDimCore *>(param0);

	IOService *display = self->testDisplay;
	if (!display)
		return;

	uint32_t stage = self->testStage++;
	if (stage == 0) {
		AuxDimTest::run(display, AuxDimTest::Backend::Intel);
		if (self->testVesa) {
			self->armTest(TestGapSeconds);
			return;
		}
		AuxDimProbe::publish("test-armed", "done");
		self->testDisplay = nullptr;
		display->release();
		return;
	}

	AuxDimTest::run(display, AuxDimTest::Backend::Vesa);
	AuxDimProbe::publish("test-armed", "done");
	self->testDisplay = nullptr;
	display->release();
}

void AuxDimCore::statusEntry(thread_call_param_t param0, thread_call_param_t) {
	auto *self = static_cast<AuxDimCore *>(param0);
	AuxDimProbe::publishf("brightness-events", "%u", self->brightnessEvents);
	AuxDimProbe::publishf("brightness-last", "brightness=%u linear-brightness=%u uptime=%us",
						 self->lastBrightness, self->lastLinearBrightness, AuxDimProbe::uptimeSeconds());
	if (self->ctlEnabled)
		AuxDimProbe::publishf("ctl-range-misses", "%u", self->ctlRangeMisses);
}
