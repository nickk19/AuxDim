//
//  kern_dpaux.hpp
//  AuxDim
//
//  DisplayPort AUX (DPCD) access routed through the framebuffer driver's own
//  IOI2CInterface, so no GPU registers are touched directly and the code does
//  not depend on a specific GPU generation or vendor.
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_dpaux_hpp
#define kern_dpaux_hpp

#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <IOKit/i2c/IOI2CInterface.h>

namespace DPAux {

	/**
	 *  Values of the IOI2CBusType registry property (see IOKit/i2c/IOI2CInterface.h
	 *  in Apple's IOGraphics sources). Informative only: Apple's Intel framebuffer reports
	 *  BusTypeI2C even for DisplayPort connectors, so native AUX support is decided by the
	 *  transaction type mask alone.
	 */
	static constexpr uint32_t BusTypeI2C         {1};
	static constexpr uint32_t BusTypeDisplayPort {2};

	/**
	 *  Largest payload of a single native AUX transaction.
	 */
	static constexpr uint32_t MaxTransfer {16};

	/**
	 *  One IOI2CInterface published below a framebuffer.
	 */
	struct Interface {
		IOI2CInterface *service {nullptr}; // retained, release with release()
		uint32_t busType {0};              // IOI2CBusType, 0 when absent
		uint32_t txTypes {0};              // IOI2CTransactionTypes bitmask, 0 when absent
		uint64_t id {0};                   // IOI2CInterfaceID, 0 when absent

		/**
		 *  Whether the interface advertises DisplayPort native (AUX) transactions.
		 */
		bool supportsNativeAux() const;
	};

	/**
	 *  Walk up the provider chain of a display (IODisplay -> IODisplayConnect -> IOFramebuffer)
	 *
	 *  @param display  IODisplay (or any service below a framebuffer)
	 *
	 *  @return retained IOFramebuffer or nullptr, release when done
	 */
	IOService *copyFramebuffer(IOService *display);

	/**
	 *  Collect every IOI2CInterface attached to a framebuffer.
	 *
	 *  @param framebuffer  framebuffer service
	 *  @param out          output array, entries are retained
	 *  @param max          capacity of out
	 *
	 *  @return number of entries written
	 */
	size_t collectInterfaces(IOService *framebuffer, Interface *out, size_t max);

	/**
	 *  Release the services held by an array filled by collectInterfaces.
	 */
	void releaseInterfaces(Interface *list, size_t count);

	/**
	 *  Find the first interface below a display's framebuffer that advertises native AUX.
	 *
	 *  @param display  IODisplay (or any service below a framebuffer)
	 *
	 *  @return retained interface or nullptr, release when done
	 */
	IOI2CInterface *copyNativeAux(IOService *display);

	/**
	 *  Read DPCD bytes. Splits the request into MaxTransfer sized transactions.
	 *
	 *  Must not be called from a context that holds the framebuffer lock
	 *  (e.g. from inside an IODisplay parameter callback).
	 */
	IOReturn read(IOI2CInterface *aux, uint32_t address, uint8_t *buffer, uint32_t length);

	/**
	 *  Write DPCD bytes. Same constraints as read().
	 */
	IOReturn write(IOI2CInterface *aux, uint32_t address, const uint8_t *buffer, uint32_t length);
}

#endif /* kern_dpaux_hpp */
