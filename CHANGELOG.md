# Changelog

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
  was dropped silently. Both shapes are handled now (power handling passes the whole dictionary), and a request without a
  usable range is counted in `ctl-range-misses`.
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
