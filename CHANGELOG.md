# Changelog

## 0.3.0 - 2026-11-10

#### Added
- The probe also reads `0x300`-`0x32F`: the source OUI (`source-oui`, decoded as AMD, Intel or none) and AMD's backlight registers
  (`amd-state`: panel caps at `0x317`, level in millinits at `0x320`, current level at `0x326`/`0x32A`, control and enable at
  `0x32E`/`0x32F`). Still reads only.
- `-auxdimamd`, a backend for AMD's backlight interface, the one the Linux amdgpu driver uses for OLED panels: it
  switches the interface on (`0x32F`), writes the level in millinits to `0x320` and sets `0x32E`, and reads the current level back
  from `0x32A`. Only for OLED panels that advertise it (`0x317`). Modelled on a Linux register dump of an ASUS UM325U with a Samsung
  `ATNA33XC11-0` (issue #1), and tested on that machine under macOS.
- `ctl-timing`: how long the AUX transfers of each write take (last, average, maximum). They hold the framebuffer lock that every
  brightness request from macOS also takes, so slow transfers would make the slider wait.

#### Changed
- The panel I/O is split into backends (`kern_backend_*.cpp`), each with its own list of registers it may write; the controller
  talks to the panel only through the backend it was given. Before the first write to a display the backend checks that the
  panel takes its level from AUX, and `ctl-activate` reports what it found.
- Brightness control is enabled by choosing a backend with its own boot argument: `-auxdimintel` for the Intel nits interface.
  Exactly one may be given; with more than one, control stays off and `ctl-state` says why. `auxdimnits` no longer enables control
  by itself: it, `auxdimminnits` and `auxdimmaxnits` only apply together with a backend.
- The default floor is 0 nits instead of 10: the lowest slider position darkens the panel completely, as on a Mac.
  `auxdimminnits=N` keeps the lowest position visible.

#### Removed
- `-auxdimctl`: replaced by `-auxdimintel`.

## 0.2.0 - 2026-11-09

#### Added
- The level is written again when the display comes back on after being off (the `dsyp` display parameter), once `0x344` shows
  that the panel still takes its brightness from AUX; `ctl-wake` reports the result and `power-last` the display power states
  macOS set. If a wake leaves AUX brightness off, nothing is written and `ctl-wake` says so.

#### Changed
- The controller keeps the panel's AUX interface instead of walking the registry for it on every write, and looks it up again
  after a failed transfer or when the interface goes away. Writes and read-backs no longer overlap on the AUX channel.
- The brightness counters (`brightness-*`, `power-last`, `ctl-range-misses`, `ctl-foreign`) are refreshed as properties only:
  they no longer write a log line after every slider move.
- Without `-auxdimctl` or `-auxdimprobe` the plugin no longer hooks IOGraphicsFamily at all (the Debug build still does with
  `-auxdimdbg`, to log the requests).
- `bootstrap.sh` pins its dependencies: MacKernelSDK to a fixed commit (`SDK_COMMIT` overrides it) and Lilu to 1.7.2 instead of
  the latest release, with the archive checked against its SHA-256. Another `LILU_VERSION` still works, with a warning that it
  is not verified.
- `BRTKEYS-patches.plist`: the renames now start at the EC device (`Base` = `\_SB.PC00.LPCB.EC0`) instead of anywhere in the DSDT.
  `Docs/BRIGHTNESS-KEYS.md` explains how to read the method header from your own DSDT (one or two length bytes), how `Base`
  behaves and how to count the matches.

#### Fixed
- The top of the luminance range was taken from the level found in the panel when the EDID had no usable luminance. A panel left
  at a low level (or reporting 0 or 0xFFFF) would then get a range of a few nits, or 0 nits at the top of the slider. The top now
  comes only from the EDID frame-average luminance (as in the Linux driver) or from the new `auxdimmaxnits=N`; without either
  the controller stays inert and says why. The floor can no longer be 0.
- Two brightness writes could run in parallel (a thread call armed again while running starts a second instance), so an older
  level could reach the panel last and stay there until the next change. All thread calls are now created with
  `THREAD_CALL_OPTIONS_ONCE`.
- A failed write was retried only while fewer than five failures had piled up since the last success, so after one bad spell
  (framebuffer not powered yet) later requests were never retried, and a request made while the framebuffer was still off was
  lost after five seconds. Every request now gets its own retries, after 1, 2, 4, 8 and 16 seconds, and a new request goes out
  at once even while a retry is waiting; `ctl-last` shows them.
- Control was lost until a reboot if the framebuffer terminated the panel's IODisplay and created a new one: requests from the new
  display were ignored as coming from another monitor. The controller now detaches from a display that went away and moves to its
  replacement once the probe has vetted it. Displays are told apart by registry ID, so a new one at the old address is probed too.
- Before the first attach, a request from an external monitor replaced the panel's waiting request (the saved brightness at
  boot). A display without a backlight can no longer displace the panel's request; requests from displays that are not driven
  are counted in `ctl-foreign`. The boot level `auxdimnits` is applied to the first display only.
- README and `Docs/PROBING.md` said that the Debug build needs a Debug Lilu to show its extra lines. It does not; only
  `liludump` needs a Debug Lilu.
- If Lilu could not take the request to watch IOGraphicsFamily, the plugin panicked the kernel (`onKextLoadForce`). It now logs
  the error and stays inert.
- README: the minimum Lilu is 1.7.1 (the first release that loads on macOS 26 without `-lilubetaall`), not 1.2.0, which
  lacks APIs the plugin uses. Built and tested with 1.7.2.
- `SSDT-PNLF.dsl`: the comment on `_UID` said Apple's backlight code reads it; it is WhateverGreen that does (the PWM range it
  programs on Intel graphics). The compiled table is unchanged.
- Documentation and comments: `0x344` bit 4 is called what it is, brightness through AUX enabled (`BRIGHTNESS_AUX_ENABLE` in
  Linux), not "nits mode"; `kIOReturnOffline` means the framebuffer is off, not necessarily the panel; the power handling only
  sends `brightness` on displays without `dsyp`; the advice for a screen left too dark no longer claims that a reboot helps;
  the README says the plugin is tested only on Tahoe and no longer points to an issue template that does not exist.

#### Removed
- The write test (`-auxdimtest`, `-auxdimtestvesa`, `auxdimtestdelay`). It served to find the register that dims the first panel;
  `auxdimnits=N` now checks the write path on its own. The VESA level register is no longer writable at all: the only write the
  plugin can make is the Intel level block at `0x354`.

## 0.1.0 - 2026-11-06

#### Added
- Project scaffold: Lilu plugin, command line build (`Makefile`, no Xcode needed), SDK bootstrap script.
- Phase 0 diagnostics: observes brightness requests at `IODisplay::doIntegerSet` and, with
  `-auxdimprobe`, reads (never writes) display, AUX transport and backlight DPCD information.
- Probe decodes both backlight interfaces (`intel-state`, `vesa-state`), the EDID data string name and the HDR luminance
  (`edid-luminance`); counts brightness requests (`brightness-events`, `brightness-last`).
- `-auxdimtest`: opt-in write test that dims the panel through the Intel nits level register and restores it; `-auxdimtestvesa`
  adds the VESA level registers.
- `Resources/ACPI/SSDT-PNLF`: generic `backlight` device so macOS shows the slider and sends brightness requests.
- `Resources/ACPI/SSDT-BRTKEYS` and `BRTKEYS-patches.plist`: example of getting the brightness keys to macOS on laptops where the
  firmware answers them with an ACPI notification for Windows (EC query methods): the SSDT forwards them to VoodooPS2Keyboard.
  Query numbers and paths are firmware specific, `Docs/BRIGHTNESS-KEYS.md` explains how to adapt them.
- Phase 1, `-auxdimctl`: the brightness macOS asks for is applied to the panel through the Intel nits interface. Requests are
  recorded in the `IODisplay::doIntegerSet` hook and applied from a thread call (throttled to one write per 30 ms, newest value
  wins, retried when the panel does not answer). The value range is read from the parameter itself; the mapping keeps the result
  between a floor (`auxdimminnits`, default 10 nits) and the EDID average luminance. `auxdimnits=N` sets a fixed level at boot.
  Results are published as `ctl-state` / `ctl-last` on the `AuxDim` service. See `Docs/CONTROL.md`.

#### Changed
- The project is called AuxDim (kext `AuxDim.kext`, bundle id `io.github.nickk19.AuxDim`). Boot arguments use the prefix `auxdim`:
  `-auxdimctl`, `-auxdimprobe`, `-auxdimtest`, `auxdimnits=N`, `auxdimminnits=N` and so on.
- `ctl-last` no longer reports a read back taken right after the write (the panel is still ramping, it showed an intermediate level);
  the panel is read once, 0.6 s after the last write, and reported as `ctl-verify`.

#### Fixed
- Brightness requests never reached the controller: the hook looked for the parameter's range inside the `params` argument of
  `IODisplay::doIntegerSet`, but `setProperties()` passes the parameter's own entry (`{min, max, value}`), so every request
  was dropped silently. Both shapes are handled now (the power handling of a display without a `dsyp` parameter passes the
  whole dictionary), and a request without a usable range is counted in `ctl-range-misses`.
- Tahoe's slider only moves `linear-brightness`; `brightness` keeps its start value. After the first `linear-brightness`
  request plain `brightness` is ignored, so an old value sent again cannot override the slider.
- With the SSDT-PNLF the controller stayed inert: macOS sets the brightness at the very start, the one-shot probe then ran while
  the framebuffer still answered every AUX read with `kIOReturnOffline`. The probe is now repeated while the panel does not
  answer, and `ctl-state` says why the controller is waiting or inert.
- The kext was rejected by the kernel: `OSBundleCompatibleVersion` (1.0) was higher than the bundle version (0.1.0).
- Native AUX detection no longer requires `IOI2CBusType` 2 (Apple's Intel framebuffer reports 1).
- Intel nits level is written as the 4 byte block the Linux driver uses (a 2 byte write was acknowledged but ignored by the panel).
  Confirmed on the first target panel: the screen dims and the level can be restored.
- The write test no longer reports "finished with errors" when the panel reads back one nit below what was written (399 written,
  398 read), which is how this panel behaves.
