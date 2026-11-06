# Notices and acknowledgements

## Build scaffolding

The compiler and linker settings of the `Makefile`, the kernel module descriptor
template, the archive step and the plugin entry point follow
the conventions of the Lilu plugin ecosystem (notably
[WhateverGreen](https://github.com/acidanthera/WhateverGreen)). Those
parts are derived from work distributed under the following license:

```
Copyright (c) 2016-2018, vit9696
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Dependencies (not redistributed)

- [Lilu](https://github.com/acidanthera/Lilu), BSD 3-Clause, fetched by `bootstrap.sh`.
- [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK), fetched by `bootstrap.sh`.

## Prior art and references

- [gil-666/DELL-XPS-OLEDBrightness](https://github.com/gil-666/DELL-XPS-OLEDBrightness)
  showed that AUX based brightness (Intel HDR TCON, DPCD `0x354`) works for
  Samsung OLED eDP panels on macOS. The repository ships no license, so **no
  code was reused**; this project is an independent implementation based on
  the public specifications below.
- VESA Embedded DisplayPort (eDP) Standard, backlight control registers
  (DPCD `0x700`-`0x72F`).
- Linux kernel `drivers/gpu/drm/display/drm_dp_helper.c`,
  `drivers/gpu/drm/i915/display/intel_dp_aux_backlight.c`: register layouts
  were used as documentation only.
- Apple IOGraphics sources (APSL), read to understand `IODisplay`,
  `IOFramebuffer` and `IOI2CInterface`: <https://github.com/apple-oss-distributions/IOGraphics>
