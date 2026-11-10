# Brightness control (Phase 1)

With a backend argument the plugin applies the brightness macOS asks for to the panel, through that backend's interface:
`-auxdimintel` uses the Intel HDR/SDR nits interface (DPCD `0x354`); `-auxdimamd` uses AMD's backlight interface
(DPCD `0x320`, level in millinits), the one the Linux amdgpu driver uses for OLED panels. Give exactly one; with more than one,
control stays off. It is opt-in, off by default, and it was written for one machine (Lenovo IdeaPad Slim 5 14IAH8, Samsung OLED
`ATNA40CT01-0`); other panels are expected to need the probe first, see [PROBING.md](PROBING.md).

## How it works

1. The `IODisplay::doIntegerSet` hook sees every display parameter. For `brightness` and `linear-brightness` it records the value
   and the range of that very parameter (min, max). It does no I/O, because the framebuffer lock is held. On Tahoe the slider
   only moves `linear-brightness` (0...65403 here); `brightness` keeps its start value, so after the first linear request plain
   `brightness` is ignored.
2. A thread call, at most once every 30 ms, takes the newest request and maps it to nits: `linear-brightness` linearly, `brightness`
   with a square law (a perceptual slider position), always inside `floor ... max`. The floor is 0 nits unless `auxdimminnits` raises it, the top
   is the EDID frame-average luminance (400 nits on this panel), the same value the Linux driver uses. Without a plausible value
   there (100 to 1000 nits) the controller stays inert unless `auxdimmaxnits=N` gives it: the level found in the panel is never
   taken as the maximum, because the firmware may have left it anywhere.
3. The level is written through the framebuffer's native AUX interface. Intel: the 4 byte block `[LSB, MSB, 0, 0]` to `0x354`.
   AMD: the level in millinits with a 50 ms transition, 6 bytes to `0x320`, then `1` to `0x32E`, as amdgpu does. Before the first
   write to a display the backend checks that the panel takes its level from AUX (`ctl-activate`): the Intel backend reads
   `0x344` and never writes it, the AMD backend switches its interface on (`0x32F` = 1) if it is off, as amdgpu does when it powers
   the panel up. Once the slider has been still for 0.6 s the level is read back (`ctl-verify`; AMD: the current average at
   `0x32A`); the panel needs that long to ramp.
   If the write fails (framebuffer not powered yet, at boot or just after wake) the level is kept and retried after 1, 2, 4, 8 and
   16 seconds; a new request replaces it and starts the count again.
4. The controller attaches only to a display whose interface the probe found usable for the backend. Intel: advertised, enabled
   by the firmware (`0x344` bit 4) and readable (`intel-state` in the probe output). AMD: brightness over AUX advertised by an OLED
   panel (`0x317`, `amd-state`). On any other display it stays inert and says so in `ctl-state`. Requests from other
   displays are ignored (`ctl-foreign`); before the first attach, a display without a backlight (an external monitor) cannot
   displace the panel's waiting request. If the framebuffer terminates the panel's IODisplay and creates a new one, the
   controller detaches and moves to the new one as soon as the probe has vetted it (`ctl-state` says which).
5. Right at boot the framebuffer driver can answer every AUX read with `kIOReturnOffline` (`0xe00002d7`): the panel is not up yet.
   With the SSDT-PNLF macOS sets the brightness very early, so the probe, which starts three seconds after the first
   request, can hit exactly that window. In that case the probe is repeated (every 5 s for the first minute, then every
   30 s, 40 attempts in all) and the brightness macOS asked for in the meantime is applied as soon as the panel answers.
6. When the display comes back on after having been off (the `dsyp` parameter goes from off to on; the dimming before display
   sleep does not count), the level is written again after 0.2 s, with the same retries, after the same check as before the first
   write. Intel: if `0x344` bit 4 is clear the level is not written, since the panel would ignore it; turning the bit back on is
   not done yet. AMD: `0x32F` is set again if the panel lost it. `ctl-wake` reports what was found. A display without a `dsyp` parameter gets no such restore
   (`power-last` never appears).

The slider and the brightness keys only exist in macOS when the panel has a `backlight` device, which on this kind of machine
means [SSDT-PNLF](../Resources/ACPI). The plugin does not create it.

## Setup

1. Get the kext (a release zip, or build it with `make`, see the [README](../README.md)) and copy it into `EFI/OC/Kexts`, after Lilu.
2. Put `Resources/ACPI/SSDT-PNLF.aml` into `EFI/OC/ACPI` and add it to `config.plist` under `ACPI -> Add`. Check first that no
   other SSDT already defines a `PNLF` / `backlight` device (`ioreg -l | grep -i -E "PNLF|backlight"`); two of them conflict.
3. Boot arguments: `-auxdimintel` on Intel laptops, `-auxdimamd` on AMD laptops whose probe shows `amd-state` with
   `sdr-via-aux=yes` and `oled=yes` (add `-auxdimdbg` for the Debug build's logging).
4. Reboot. The slider should now appear in System Settings > Displays and the panel should follow it.

Optional: `auxdimminnits=N` for a different floor, `auxdimnits=N` to force a level at boot (useful to check the path without the slider),
`auxdimmaxnits=N` for a panel whose EDID has no luminance range (use the panel's specified full SDR brightness).

## What it publishes

```bash
ioreg -rn AuxDim -l -w0 | grep -E '"(ctl-|brightness-|power-|intel-state|hint-|display-class)'
ioreg -rc IODisplay -l -w0
```

| Property | Meaning |
|---|---|
| `ctl-state` | `ready: ... floor...max nits ...` once attached; `waiting: ...` while the panel does not answer (with the error code and the attempt number); `inert on this display: <reason>` when it can never attach (no AUX interface, Intel nits interface not advertised, brightness through AUX not enabled in `0x344`, no luminance range in the EDID, gave up); `detached: ...` when the display went away and its replacement is not vetted yet |
| `ctl-last` | The last applied request: parameter, value, range, resulting nits, `ret` of the write, counters |
| `ctl-verify` | What the panel itself reports about 0.6 s after the last write (it reads one nit below the value written, that counts as "as expected") |
| `ctl-timing` | How long the AUX transfers of a write take (last, average, maximum). The framebuffer lock is held that long, and macOS waits for it with every brightness request |
| `ctl-activate` | The check before the first write to a display: the backend's mode register (`0x344` for Intel, `0x32F` for AMD), whether the backend switched it on, and whether the panel takes its level from AUX |
| `ctl-wake` | The last wake: the backend's mode register and whether the level was written again (or why not) |
| `brightness-events`, `brightness-last` | How many brightness requests macOS sent, and the last values |
| `power-last` | The last display power state macOS set (`dsyp`: 0 off, 1 dimmed, 2 on) and how many changes so far |
| `ctl-range-misses` | Requests dropped because they carried no min/max. Must stay 0 |
| `ctl-foreign` | Brightness requests from displays the controller does not drive (an external monitor) |

`ctl-last` and `ctl-verify` are written once the slider has settled, not on every step.

## What to check and report

- Does the slider appear, and does the panel follow it, in both directions?
- Is the lowest notch usable (not too dark, not still too bright)? The floor and the curve are guesses until someone tries them.
- Does the brightness survive sleep and wake? If the panel comes back at the wrong level, say whether moving the slider fixes it.
  Attach `ctl-wake` and `power-last` after a display sleep (`pmset displaysleepnow`) and after a system sleep.
- Do the brightness keys work? They depend on the keyboard stack and the firmware, not on this plugin; if they do nothing, see [BRIGHTNESS-KEYS.md](BRIGHTNESS-KEYS.md).
- Attach the output of the two commands above and `ctl-last` after moving the slider to the minimum and to the maximum.

If the screen ever ends up too dark: with the default floor of 0 nits the lowest slider position darkens the panel completely,
as on a Mac, and the brightness up key brings it back without seeing the screen. `auxdimminnits=N` keeps the lowest position
visible. A reboot alone does not help, because macOS sets its saved brightness again at boot.
`auxdimnits=N` forces a level at boot that stays until the slider is moved; removing the backend argument (or adding `-auxdimoff`)
disables the plugin.
