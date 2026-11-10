//
//  kern_backend.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/kern_api.hpp>

#include "kern_backend.hpp"
#include "kern_dpaux.hpp"

namespace {

	struct Choice {
		const char *argument;
		const Backend::Ops *backend;
	};

	// One boot argument per backend. Keep Backend::arguments and the README in sync.
	const Choice choices[] = {
		{ "-auxdimintel", &Backend::intel },
		{ "-auxdimamd", &Backend::amd },
	};
}

const char *const Backend::arguments = "-auxdimintel, -auxdimamd";

const Backend::Ops *Backend::fromBootArgs(size_t &count, char *given, size_t givenSize) {
	count = 0;
	if (given && givenSize)
		given[0] = '\0';

	const Ops *selected = nullptr;
	size_t used = 0;
	for (const auto &choice : choices) {
		if (!checkKernelArgument(choice.argument))
			continue;
		if (given && used + 1 < givenSize) {
			int n = snprintf(given + used, givenSize - used, "%s%s", count ? ", " : "", choice.argument);
			if (n > 0)
				used += static_cast<size_t>(n);
		}
		count++;
		selected = choice.backend;
	}
	return count == 1 ? selected : nullptr;
}

IOReturn Backend::writeRegister(IOI2CInterface *aux, const Register *allowed, size_t count, uint32_t address,
								const uint8_t *data, uint32_t length) {
	for (size_t i = 0; i < count; i++) {
		if (allowed[i].address == address && allowed[i].length == length)
			return DPAux::write(aux, address, data, length);
	}
	SYSLOG("auxdim", "refusing DPCD write to 0x%x (%u bytes)", address, length);
	return kIOReturnNotPermitted;
}
