//
//  kern_edid.cpp
//  AuxDim
//
//  Copyright © 2026 nickk19. All rights reserved.
//  Licensed under the BSD 3-Clause License, see LICENSE.
//

#include "kern_edid.hpp"

namespace {

	// 2^(i/32) in 10 bit fixed point, i = 0...31 (no floating point in the kernel).
	const uint16_t pow2Table[32] = {
		1024, 1046, 1069, 1093, 1117, 1141, 1166, 1192, 1218, 1244, 1272, 1300, 1328, 1357, 1387, 1417,
		1448, 1480, 1512, 1545, 1579, 1614, 1649, 1685, 1722, 1760, 1798, 1838, 1878, 1919, 1961, 2004
	};

	/**
	 *  CTA-861 luminance coding: 50 * 2^(code / 32) cd/m2.
	 */
	uint32_t decodeLuminance(uint8_t code) {
		uint64_t value = (50ULL * pow2Table[code & 31]) << (code >> 5);
		return static_cast<uint32_t>((value + 512) >> 10);
	}
}

void EDID::name(const uint8_t *edid, size_t length, char *out, size_t outSize) {
	if (!out || outSize == 0)
		return;
	out[0] = '\0';
	if (!edid || length < 128)
		return;

	static const uint8_t tags[] = { 0xFC, 0xFE };
	for (uint8_t tag : tags) {
		// The four 18 byte descriptors of the base block start at offset 54.
		for (size_t off = 54; off <= 108; off += 18) {
			if (edid[off] != 0 || edid[off + 1] != 0 || edid[off + 2] != 0 || edid[off + 3] != tag)
				continue;

			size_t pos = 0;
			for (size_t i = 0; i < 13 && pos + 1 < outSize; i++) {
				char c = static_cast<char>(edid[off + 5 + i]);
				if (c == '\n' || c == '\0')
					break;
				out[pos++] = (c >= 32 && c < 127) ? c : '?';
			}
			while (pos > 0 && out[pos - 1] == ' ')
				pos--;
			out[pos] = '\0';
			if (pos > 0)
				return;
		}
	}
}

bool EDID::hdrLuminance(const uint8_t *edid, size_t length, Luminance &out) {
	out = Luminance();
	if (!edid || length < 256)
		return false;

	size_t extensions = edid[126];
	for (size_t block = 1; block <= extensions && (block + 1) * 128 <= length; block++) {
		const uint8_t *ext = edid + block * 128;
		if (ext[0] != 0x02) // CTA-861 extension
			continue;

		// ext[2] is the offset of the first detailed timing, the data blocks sit between byte 4 and it.
		size_t end = ext[2];
		if (end < 4 || end > 127)
			continue;

		for (size_t pos = 4; pos < end;) {
			uint8_t tag = ext[pos] >> 5;
			size_t len = ext[pos] & 0x1F;
			if (pos + 1 + len > end)
				break;

			// Tag 7 is the "use extended tag" block, extended tag 6 is HDR static metadata:
			// [ext tag][EOTF][static metadata types][max][max frame average][min], the last three optional.
			if (tag == 7 && len >= 4 && ext[pos + 1] == 6) {
				out.peak = decodeLuminance(ext[pos + 4]);
				if (len >= 5)
					out.average = decodeLuminance(ext[pos + 5]);
				if (len >= 6) {
					// min = max * (code / 255)^2 / 100
					uint64_t code = ext[pos + 6];
					out.minMilli = static_cast<uint32_t>(
						(static_cast<uint64_t>(out.peak) * 1000ULL * code * code) / (255ULL * 255ULL * 100ULL));
				}
				return true;
			}
			pos += 1 + len;
		}
	}

	return false;
}
