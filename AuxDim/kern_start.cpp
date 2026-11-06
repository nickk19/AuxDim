//
//  kern_start.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

#include "kern_auxdim.hpp"

static AuxDimCore auxdim;

static const char *bootargOff[] {
	"-auxdimoff"
};

static const char *bootargDebug[] {
	"-auxdimdbg"
};

static const char *bootargBeta[] {
	"-auxdimbeta"
};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::Catalina,
	KernelVersion::Tahoe,
	[]() {
		auxdim.init();
	}
};
