# Architecture

Status: design document plus the Phase 0 (read-only probe) and Phase 1 (brightness control) implementation. Statements are tagged:

- **[verified]** read in source code (Apple's open source IOGraphics, Lilu, WhateverGreen, the reference kext) or checked by a compile
- **[assumed]** plausible but not yet confirmed on real hardware; Phase 0 exists to settle these

## 1. Goals and non-goals

**Goals.** Make the macOS brightness slider and keys change the real luminance of panels where the normal backlight PWM path does
nothing: OLED eDP panels dimmed through the DisplayPort AUX channel, and later external monitors dimmed through DDC/CI. The
plugin must not depend on the GPU vendor or generation, nor on the Hackintosh graphics patches in use (WhateverGreen, WhateverXe,
NootedGreen, AMD or NVIDIA stacks). Lilu is the only dependency.

**Non-goals.** Software gamma dimming (a user space job, no kext needed), HDR and tone mapping, burn-in mitigation, and replacing
the PWM backlight patches of WhateverGreen for panels that do have a PWM backlight.

## 2. The reference implementation and why it is not agnostic

[gil-666/DELL-XPS-OLEDBrightness](https://github.com/gil-666/DELL-XPS-OLEDBrightness) dims a Samsung ATNA56WR04 OLED by writing
nits to DPCD `0x354` over the AUX channel. That protocol part is solid and is reused here as knowledge (the sequence is the same
one the Linux i915 driver uses for Intel HDR TCON panels). The plumbing around it is what ties it to one machine **[verified]**:

| Aspect | In the reference | Consequence |
|---|---|---|
| GPU selection | PCI `8086:3E9B` hard-coded | Does nothing on any other GPU |
| Register access | Maps BAR0 and pokes AUX channel A (`0x64010...`) and BLC PWM (`0xC8258`), Cannon Point PCH offsets | Offsets differ per generation; no coordination with the driver's own AUX users |
| Input | Polls `BLC_PWM_DUTY` every 33 ms, expecting WhateverGreen to have written it | Needs Intel + WhateverGreen's backlight path; polling latency |
| Panel | Model string and a 440 nit fallback hard-coded | One panel |
| Packaging | Standalone kext, no Lilu, no OS gating, no boot arguments | No off switch, no debug switch |
| License | No license file | The code cannot be reused, this project reimplements from public specs |

## 3. Layered design

```
 macOS slider / keys -> DisplayServices -> IODisplay::setProperties
        |
        v
 L1  Input     IODisplay::doIntegerSet("brightness", v)   hook, independent of the GPU driver
        |
        v
 L2  Policy    range mapping, curve, floor, level written again on wake (Phase 1); fades (later)
        |
        v
 L3  Backend   chosen by the user with its boot argument, checked by the probe (Phase 1: Intel nits)
                 - VESA eDP AUX backlight   DPCD 0x720-0x723
                 - Intel HDR/SDR TCON nits  DPCD 0x340-0x35F (0x354)
                 - AMD source backlight     DPCD 0x317, 0x320-0x32F (millinits)  (Phase 1, experimental)
                 - DDC/CI luminance         VCP 0x10  (external monitors)    (Phase 3)
        |
        v
 L4  Transport IOI2CInterface (DisplayPort native / I2C), owned by the GPU driver
```

### L1: where to listen

`IODisplay::setProperties()` calls `setForKey()` which calls the virtual `doIntegerSet()`; `AppleBacklightDisplay::doIntegerSet()`
ends in `IODisplay::doIntegerSet()`. That function finally walks the registered `IODisplayParameterHandler` objects and stops at the first
one that accepts the value **[verified in IOGraphics source]**. Two consequences:

- Registering our own `IODisplayParameterHandler` is not enough: the GPU driver's handler is presumably registered first and
  would consume brightness before ours runs **[assumed]**. Hence the Lilu hook on `IODisplay::doIntegerSet`.
- The hook runs with the framebuffer lock held **[verified]**, so it only records the request. AUX I/O is deferred to a thread
  call (the probe already works this way).
- The `params` argument is not always the same thing **[verified in IOGraphics source]**: `setProperties()` passes the entry of the
  parameter itself (`{min, max, value}`, from `getIntegerRange()`), the power handling (`updatePowerParam()`) passes the whole
  parameter dictionary. The range has to be looked up in both shapes. The power handling only sends `brightness` on a display
  without a `dsyp` (display power) parameter; a display that has one gets `dsyp` instead, which the controller watches to
  write the level again after a wake.
- On Tahoe the slider only moves `linear-brightness` **[verified, one machine]**; `brightness` keeps the value it had at start.

By contrast, WhateverGreen applies brightness one level lower, in `AppleIntelFramebufferController::hwSetBacklight`
**[verified in WhateverGreen source]**, which is why it is Intel specific. Hooking in IOGraphicsFamily keeps us above every
GPU driver, and external displays pass through the same function.

### L4: how to talk to the panel

`IOFramebuffer` publishes one `IOI2CInterface` per I2C or AUX bus. A request with transaction type
`kIOI2CDisplayPortNativeTransactionType` (4) is a native AUX transaction, the same mechanism IOGraphics itself uses to read
the DisplayPort link status **[verified]**. Going through it, instead of mapping GPU registers, means:

- the driver serialises our transfers with its own AUX traffic (`IOFramebufferI2CInterface::startIO` takes the framebuffer lock)
- the request is refused with `kIOReturnOffline` while the framebuffer is not powered or is going away, instead of touching dead
  hardware (it says nothing about the panel itself: during display sleep the panel may well stay powered)
- no per-GPU register offsets, no vendor PCI IDs

Whether a given driver advertises the native AUX bit is Open Question 2. **[verified, one machine]** AppleIntelFramebuffer under
WhateverXe on a Lenovo IdeaPad Slim 5 14IAH8 publishes one `IOFramebufferI2CInterface` per framebuffer with `IOI2CBusType` 1 (plain
I2C, not 2) and `IOI2CTransactionTypes` `0x13`, i.e. the DisplayPort native bit (`0x10`) is set. The bus type therefore cannot be used
to recognise AUX capable interfaces; the transaction mask is. **[verified, one machine, issue #1]** NootedRed on an ASUS UM325U
(Ryzen 5 5500U) publishes interfaces with bus type 2 and transaction types `0x1f`, and DPCD reads through them return the panel's
data. Unknown for other stacks.

The plugin references no symbol from IOGraphicsFamily **[verified on the compiled objects]**: classes are matched by name
(`metaCast("IOI2CInterface")`), the I2C calls are virtual, and the hook target is resolved by Lilu at runtime. A missing symbol
therefore makes the plugin inert and logs a line; it cannot stop the kext from linking.

## 4. Open questions and what the first hardware report says

First report: Lenovo IdeaPad Slim 5 14IAH8, Samsung OLED panel `ATNA40CT01-0`, WhateverXe, macOS Tahoe. All findings below are
**[verified, one machine]**.

| # | Question | Answer |
|---|---|---|
| 1 | Does Tahoe's `IOGraphicsFamily` still export `IODisplay::doIntegerSet`? | Yes. The hook was routed and fired for the `AppleDisplay` (the probe is triggered by it). |
| 2 | Does the driver publish a DisplayPort-native `IOI2CInterface` for the eDP bus? | Yes, and DPCD reads through it succeed from a kext (bus type 1, transaction types `0x13`). |
| 3 | Which backlight interface does the panel implement? | Both. Intel HDR TCON v1: nits capable, SDR brightness via AUX, brightness through AUX already enabled by the firmware (`0x344` = `0x10`: bit 4, `INTEL_EDP_HDR_TCON_BRIGHTNESS_AUX_ENABLE` in Linux), current level `0x354` = 399, which matches the 400 nit frame-average luminance in the EDID. VESA eDP 1.4b: AUX brightness and PWM pin supported, mode 3 (product), enable bit clear, level `0xFFFF`. The reference kext for another Samsung ATNA panel uses the Intel one (`0x354`). Phase 0.5 write test (since removed): AUX writes through the framebuffer's interface work (the VESA level registers took and returned every value) but VESA does not dim the panel while its enable bit is clear, and a 2 byte write to `0x354` was acknowledged but ignored. |
| 4 | Does macOS emit brightness requests? | No: the display is a plain `AppleDisplay`, `param-brightness` is `absent`. It needs the ACPI `backlight` device (`Resources/ACPI/SSDT-PNLF.dsl`). |
| 5 | Which range does macOS use? | **[verified, one machine]** With the SSDT-PNLF the display becomes an `AppleBacklightDisplay` and macOS sends two parameters: `brightness` 0...1024 (slider position) and `linear-brightness` 0...65403 (proportional to luminance); `usable-linear-brightness` starts at 655 (1%). The framebuffer's raw `bklt` is 0...65535. Phase 1 does not hard-code any of this: it reads min and max from the parameter that carried the request, prefers `linear-brightness` and publishes the range in `ctl-last`. |
| 6 | Does the TCON forget the setup after sleep? | **[verified, one machine, display sleep]** No. With WhateverGreen's `-wex-edp-blank` the panel stays powered through display sleep: macOS sends `dsyp` off and on again, after the wake `0x344` still reads `0x10`, and the level that Phase 1 writes again reads back as expected (`ctl-wake`, `ctl-verify`). System sleep is not tested. Setting `0x344` bit 4 again when a wake clears it is a write that is not made yet. |
| 7 | Which AUX write shape does the Intel nits register take? | **[verified, one machine]** A 4 byte block `[LSB, MSB, 0, 0]` at `0x354`, as the Linux driver writes it. A 2 byte write is acknowledged and ignored. The panel reads back one nit below the value written. |

Second report (issue #1): ASUS UM325U (Ryzen 5 5500U, NootedRed), Samsung OLED panel `ATNA33XC11-0`. Under macOS nothing sets a
level: `0x344` is `0x00`, the source OUI at `0x300` is empty, the VESA enable bit is clear, and the panel sits at full brightness.
A register dump under Linux **[verified, one machine]** shows how amdgpu drives it: AMD's source OUI at `0x300` (`00 00 1a`),
panel caps `0x317` = `0x17` (SDR and HDR brightness over AUX, OLED), level in millinits at `0x320` with a 50 ms transition,
control `0x32E` = 1, enable `0x32F` = 1, current level reported at `0x326`/`0x32A`. The Intel block stays disabled and the VESA
block untouched; the panel only mirrors the current level into `0x354`. Hence the AMD backend.

## 5. Safety rules for the write phase

- Write only to whitelisted backlight registers, and only after the probe confirmed the matching capability on that display.
- Never write to a display whose identity and capabilities were not probed; unknown panels stay read-only.
- Clamp to a minimum luminance so the slider can never produce a black, unrecoverable screen (Phase 1: floor of 10 nits,
  `auxdimminnits`, and the mapping can never leave the floor...maximum range).
- Rate-limit AUX writes and coalesce bursts of slider events (Phase 1: one write per 30 ms, the newest value wins).
- Write the level again on wake (Intel: only while `0x344` still enables AUX brightness; AMD: after switching its interface on
  again if needed) and keep `-auxdimoff` working at all times.
- Control is opt-in, one backend argument at a time (`-auxdimintel`, `-auxdimamd`), and only attaches to a display whose interface
  the probe found usable for that backend (Intel: advertised, enabled and readable; AMD: brightness over AUX advertised by an OLED
  panel). Any other display is left alone. A backend switches an interface on only if the GPU's own driver does the same and no
  firmware would: the AMD backend sets `0x32F`, the Intel backend never writes `0x344`.

## 6. Roadmap

| Phase | Content |
|---|---|
| 0 (done) | Observe `doIntegerSet`, read-only probe, hardware report workflow |
| 0.5 (done) | Opt-in write test of the two level registers (removed afterwards: `auxdimnits=N` checks the write path), brightness request counters; SSDT-PNLF for the slider |
| 1 (now) | Backends chosen by boot argument: Intel nits (`-auxdimintel`), and AMD (`-auxdimamd`, experimental, modelled on the Linux dump of issue #1); apply brightness from L1 events. Still to do: an opt-in to set `0x344` bit 4 again if a wake is ever seen to clear it |
| 2 | VESA backend, EDID keyed quirk table, curve and floor tuning, persistence across reboots |
| 3 | DDC/CI backend for external monitors, per-display routing |
| 4 | Polish: fades, packaging, documentation aimed at upstream Hackintosh guides |

## 7. References

- VESA Embedded DisplayPort (eDP) Standard, backlight control registers
- Linux `drm_dp_helper.h`, `intel_dp_aux_backlight.c` (register layouts as documentation)
- Apple IOGraphics: `IODisplay.cpp`, `IOBacklightDisplay.cpp`, `IOFramebuffer.cpp`, `IOKit/i2c/IOI2CInterface.h`
- Lilu and WhateverGreen sources for plugin structure and the IOGraphicsFamily routing pattern
