//
//  kern_dpaux.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/kern_api.hpp>

#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>

#include "kern_dpaux.hpp"

bool DPAux::Interface::supportsNativeAux() const {
	// The transaction mask is what matters. Apple's IOFramebufferI2CInterface reports
	// IOI2CBusType 1 (I2C) even when it advertises DisplayPort native transactions
	// (seen on AppleIntelFramebuffer: bus type 1, transaction types 0x13), so the bus
	// type is only informative.
	return service &&
		(txTypes & (1U << kIOI2CDisplayPortNativeTransactionType)) != 0;
}

IOService *DPAux::copyFramebuffer(IOService *display) {
	// Do not assume a fixed depth: IODisplay sits below IODisplayConnect which sits below the
	// framebuffer, but the exact shape is owned by IOGraphicsFamily. Match by class name so that
	// no symbol from IOGraphicsFamily is needed at link time.
	IOService *current = display;
	for (int depth = 0; current && depth < 6; depth++) {
		if (current->metaCast("IOFramebuffer")) {
			current->retain();
			return current;
		}
		current = current->getProvider();
	}
	return nullptr;
}

size_t DPAux::collectInterfaces(IOService *framebuffer, Interface *out, size_t max) {
	if (!framebuffer || !out || max == 0)
		return 0;

	auto *iter = framebuffer->getChildIterator(gIOServicePlane);
	if (!iter)
		return 0;

	size_t count = 0;
	while (count < max) {
		auto *obj = iter->getNextObject();
		if (!obj)
			break;

		// IOI2CInterface subclasses are created by IOFramebuffer itself, one per I2C/AUX bus.
		auto *entry = static_cast<OSObject *>(obj)->metaCast("IOI2CInterface");
		if (!entry)
			continue;

		auto *service = static_cast<IOI2CInterface *>(entry);
		Interface &item = out[count];
		item.service = service;
		service->retain();

		auto *bus = OSDynamicCast(OSNumber, service->getProperty(kIOI2CBusTypeKey));
		auto *tx  = OSDynamicCast(OSNumber, service->getProperty(kIOI2CTransactionTypesKey));
		auto *id  = OSDynamicCast(OSNumber, service->getProperty(kIOI2CInterfaceIDKey));
		item.busType = bus ? bus->unsigned32BitValue() : 0;
		item.txTypes = tx ? tx->unsigned32BitValue() : 0;
		item.id      = id ? id->unsigned64BitValue() : 0;
		count++;
	}

	iter->release();
	return count;
}

void DPAux::releaseInterfaces(Interface *list, size_t count) {
	for (size_t i = 0; list && i < count; i++) {
		if (list[i].service) {
			list[i].service->release();
			list[i].service = nullptr;
		}
	}
}

IOI2CInterface *DPAux::copyNativeAux(IOService *display) {
	IOService *framebuffer = copyFramebuffer(display);
	if (!framebuffer)
		return nullptr;

	Interface list[8];
	size_t count = collectInterfaces(framebuffer, list, 8);

	IOI2CInterface *found = nullptr;
	for (size_t i = 0; i < count && !found; i++) {
		if (list[i].supportsNativeAux()) {
			found = list[i].service;
			found->retain();
		}
	}

	releaseInterfaces(list, count);
	framebuffer->release();
	return found;
}

IOReturn DPAux::read(IOI2CInterface *aux, uint32_t address, uint8_t *buffer, uint32_t length) {
	if (!aux || !buffer || length == 0)
		return kIOReturnBadArgument;

	uint32_t done = 0;
	while (done < length) {
		uint32_t chunk = length - done;
		if (chunk > MaxTransfer)
			chunk = MaxTransfer;

		IOI2CRequest request;
		bzero(&request, sizeof(request));
		request.sendTransactionType  = kIOI2CNoTransactionType;
		request.replyTransactionType = kIOI2CDisplayPortNativeTransactionType;
		request.replyAddress = address + done;
		request.replyBuffer  = reinterpret_cast<vm_address_t>(buffer + done);
		request.replyBytes   = chunk;

		IOReturn ret = aux->startIO(&request);
		if (ret == kIOReturnSuccess)
			ret = request.result;
		if (ret != kIOReturnSuccess)
			return ret;

		done += chunk;
	}

	return kIOReturnSuccess;
}

IOReturn DPAux::write(IOI2CInterface *aux, uint32_t address, const uint8_t *buffer, uint32_t length) {
	if (!aux || !buffer || length == 0)
		return kIOReturnBadArgument;

	uint32_t done = 0;
	while (done < length) {
		uint32_t chunk = length - done;
		if (chunk > MaxTransfer)
			chunk = MaxTransfer;

		IOI2CRequest request;
		bzero(&request, sizeof(request));
		request.sendTransactionType  = kIOI2CDisplayPortNativeTransactionType;
		request.replyTransactionType = kIOI2CNoTransactionType;
		request.sendAddress = address + done;
		request.sendBuffer  = reinterpret_cast<vm_address_t>(const_cast<uint8_t *>(buffer + done));
		request.sendBytes   = chunk;

		IOReturn ret = aux->startIO(&request);
		if (ret == kIOReturnSuccess)
			ret = request.result;
		if (ret != kIOReturnSuccess)
			return ret;

		done += chunk;
	}

	return kIOReturnSuccess;
}
