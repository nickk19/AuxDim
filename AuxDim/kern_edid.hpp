//
//  kern_edid.hpp
//  AuxDim
//
//  Small, allocation free EDID helpers: the display name and the luminance the
//  panel declares in its HDR static metadata block.
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#ifndef kern_edid_hpp
#define kern_edid_hpp

#include <Headers/kern_util.hpp>

namespace EDID {

	/**
	 *  Display name: the monitor name descriptor (tag 0xFC) or, failing that, the display data string
	 *  descriptor (tag 0xFE). Many eDP panels only carry the latter, with the panel model code in it
	 *  (e.g. "ATNA40CT01-0"). Trailing blanks are removed.
	 *
	 *  @param edid    EDID base block and extensions
	 *  @param length  number of bytes in edid
	 *  @param out     output buffer, always NUL terminated, empty when no descriptor is present
	 */
	void name(const uint8_t *edid, size_t length, char *out, size_t outSize);

	/**
	 *  Luminance declared by the CTA-861 HDR static metadata data block.
	 */
	struct Luminance {
		uint32_t peak     {0}; // nits, "desired content max luminance"
		uint32_t average  {0}; // nits, "desired content max frame-average luminance", 0 when absent
		uint32_t minMilli {0}; // thousandths of a nit, 0 when absent
	};

	/**
	 *  Parse the HDR static metadata block from the CTA-861 extension.
	 *
	 *  @return false when the extension or the block is absent
	 */
	bool hdrLuminance(const uint8_t *edid, size_t length, Luminance &out);
}

#endif /* kern_edid_hpp */
