# Brightness control (Phase 1)

With `-auxdimctl` the plugin applies the brightness macOS asks for to the panel, through the Intel HDR/SDR nits interface
(DPCD `0x354`). It is opt-in, off by default, and it was written for one machine (Lenovo IdeaPad Slim 5 14IAH8, Samsung OLED
`ATNA40CT01-0`); other panels are expected to need the probe first, see [PROBING.md](PROBING.md).

## How it works

1. The `IODisplay::doIntegerSet` hook sees every display parameter. For `brightness` and `linear-brightness` it records the value
   and the range of that very parameter (min, max). It does no I/O, because the framebuffer lock is held. On Tahoe the slider
   only moves `linear-brightness` (0...65403 here); `brightness` keeps its start value, so after the first linear request plain
   `brightness` is ignored.
2. A thread call, at most once every 30 ms, takes the newest request and maps it to nits: `linear-brightness` linearly, `brightness`
   with a square law (a perceptual slider position), always inside `floor ... max`. The floor is 10 nits (`auxdimminnits`), the top
   is the EDID frame-average luminance (400 nits on this panel).
3. The level is written as the 4 byte block `[LSB, MSB, 0, 0]` to `0x354` through the framebuffer's native AUX interface, then read back.
   If the write fails (panel asleep, just woken) the request is kept and retried every second, up to five times.
4. The controller attaches only to a display whose Intel nits interface is advertised, enabled by the firmware and readable
   (`intel-state` in the probe output). On any other display it stays inert and says so in `ctl-state`.
5. Right at boot the framebuffer driver can answer every AUX read with `kIOReturnOffline` (`0xe00002d7`): the panel is not up yet.
   With the SSDT-PNLF macOS sets the brightness very early, so the probe, which starts three seconds after the first
   request, can hit exactly that window. In that case the probe is repeated (every 5 s for the first minute, then every
   30 s, 40 attempts in all) and the brightness macOS asked for in the meantime is applied as soon as the panel answers.

The slider and the brightness keys only exist in macOS when the panel has a `backlight` device, which on this kind of machine
means [SSDT-PNLF](../Resources/ACPI). The plugin does not create it.

## Setup

1. Get the kext (a release zip, or build it with `make`, see the [README](../README.md)) and copy it into `EFI/OC/Kexts`, after Lilu.
2. Put `Resources/ACPI/SSDT-PNLF.aml` into `EFI/OC/ACPI` and add it to `config.plist` under `ACPI -> Add`. Check first that no
   other SSDT already defines a `PNLF` / `backlight` device (`ioreg -l | grep -i -E "PNLF|backlight"`); two of them conflict.
3. Boot arguments: `-auxdimctl` (add `-auxdimdbg` for the Debug build's logging). Do not combine with `-auxdimtest`.
4. Reboot. The slider should now appear in System Settings > Displays and the panel should follow it.

Optional: `auxdimminnits=N` for a different floor, `auxdimnits=N` to force a level at boot (useful to check the path without the slider).

## What it publishes

```bash
ioreg -rn AuxDim -l -w0 | grep -E '"(ctl-|brightness-|intel-state|hint-|display-class)'
ioreg -rc IODisplay -l -w0
```

| Property | Meaning |
|---|---|
| `ctl-state` | `ready: ... floor...max nits ...` once attached; `waiting: ...` while the panel does not answer (with the error code and the attempt number); `inert on this display: <reason>` when it can never attach (no AUX interface, nits mode not advertised or not enabled, gave up) |
| `ctl-last` | The last applied request: parameter, value, range, resulting nits, `ret` of the write, counters |
| `ctl-verify` | What the panel itself reports about 0.6 s after the last write (it reads one nit below the value written, that counts as "as expected") |
| `brightness-events`, `brightness-last` | How many brightness requests macOS sent, and the last values |
| `ctl-range-misses` | Requests dropped because they carried no min/max. Must stay 0 |

`ctl-last` and `ctl-verify` are written once the slider has settled, not on every step.

## What to check and report

- Does the slider appear, and does the panel follow it, in both directions?
- Is the lowest notch usable (not too dark, not still too bright)? The floor and the curve are guesses until someone tries them.
- Does the brightness survive sleep and wake? If the panel comes back at the wrong level, say whether moving the slider fixes it.
- Do the brightness keys work? They depend on the keyboard stack and the firmware, not on this plugin; if they do nothing, see [BRIGHTNESS-KEYS.md](BRIGHTNESS-KEYS.md).
- Attach the output of the two commands above and `ctl-last` after moving the slider to the minimum and to the maximum.

If the screen ever stays too dark to use, a reboot resets the panel; removing `-auxdimctl` (or adding `-auxdimoff`) disables the plugin.
