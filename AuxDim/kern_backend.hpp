//
//  kern_backend.hpp
//  AuxDim
//
//  A backend is one way of setting an eDP panel's brightness over AUX: the Intel TCON nits interface, AMD's own
//  interface, and so on. The controller (kern_control.hpp) talks to the panel only through the functions of the
//  backend it was given, and every DPCD write the plugin makes goes through a backend's own list of allowed registers.
//  Each backend lives in its own file (kern_backend_<name>.cpp) and is selected with its own boot argument; exactly one
//  may be given.
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_backend_hpp
#define kern_backend_hpp

#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <IOKit/i2c/IOI2CInterface.h>

#include "kern_backlight.hpp"

namespace Backend {

	struct Ops {
		const char *name;          // for status lines, e.g. "Intel nits interface"
		const char *modeRegister;  // the register activate() reads, for status lines, e.g. "0x344"

		/**
		 *  Whether the panel offers this interface, judged from what the probe read. Never writes.
		 *
		 *  @param reason  why not, when it returns false
		 */
		bool (*usable)(const Backlight::Caps &caps, const char *&reason);

		/**
		 *  Make sure the panel takes its level from this interface. Called before the first write to a display and
		 *  after every wake. A backend that may switch its interface on does so here; one that may not only reports.
		 *
		 *  @param mode      the mode register as it was read, before any change
		 *  @param switched  set when this call switched the interface on
		 *  @param on        set when the panel takes its level from this interface now
		 *
		 *  @return the result of the transfers; on failure the caller retries later
		 */
		IOReturn (*activate)(IOI2CInterface *aux, uint8_t &mode, bool &switched, bool &on);

		/**
		 *  Set the panel's luminance, in nits.
		 */
		IOReturn (*write)(IOI2CInterface *aux, uint32_t nits);

		/**
		 *  The luminance the panel reports, in nits.
		 */
		IOReturn (*read)(IOI2CInterface *aux, uint32_t &nits);
	};

	/**
	 *  A register a backend may write: exactly this address and this length.
	 */
	struct Register {
		uint32_t address;
		uint32_t length;
	};

	/**
	 *  The only way to write DPCD: refused (and logged) unless address and length match an entry of allowed.
	 */
	IOReturn writeRegister(IOI2CInterface *aux, const Register *allowed, size_t count, uint32_t address,
						   const uint8_t *data, uint32_t length);

	extern const Ops intel;
	extern const Ops amd;

	/**
	 *  Every backend argument, for messages: "-auxdimintel, ..."
	 */
	extern const char *const arguments;

	/**
	 *  The backend selected on the boot command line.
	 *
	 *  @param count  how many backend arguments were given
	 *  @param given  filled with the arguments given, comma separated
	 *
	 *  @return the backend when exactly one argument was given, otherwise nullptr
	 */
	const Ops *fromBootArgs(size_t &count, char *given, size_t givenSize);
}

#endif /* kern_backend_hpp */
