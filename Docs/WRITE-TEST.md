# Write test (`-auxdimtest`)

The probe tells which backlight interfaces a panel advertises. It cannot tell which one actually changes the
luminance, nor what the level scale is. This test finds out, on the real panel, before any brightness policy is built.

It is **opt-in**: nothing is ever written unless `-auxdimtest` is in `boot-args`.

## What it does

After the probe found a display with a native AUX interface and an advertised backlight interface, the plugin waits
(`auxdimtestdelay` seconds, default 120, counted from the probe which fires early in boot) and then runs a sweep on that
display. By default only the Intel interface is swept; `-auxdimtestvesa` adds a second sweep a minute later:

| Sweep | Interface | Register written |
|---|---|---|
| 1 (default) | Intel HDR/SDR TCON nits | DPCD `0x354`...`0x357`: level LSB, MSB, delay frames and per frame steps (both zero), one 4 byte transaction, as the Linux driver does |
| 2 (`-auxdimtestvesa`) | VESA eDP AUX backlight | DPCD `0x722`/`0x723` (level MSB, LSB) |

Each sweep reads the starting level, writes 50 %, 25 % and 10 % of it (reading it back after one second and holding
each step for three more), then writes the starting level back and verifies it. For the Intel sweep the whole
`0x340`...`0x35F` block is also published after every step (`test-intel-stepN-dpcd`), to see whether anything besides
the level moved. A sweep takes about fifteen seconds. You should see the screen get darker in three steps and come back.

## Safety

- Only the two level registers can be written. The gate is `writeAllowed()` in `kern_backlight.cpp`; mode, enable and
  control registers are not reachable from the plugin.
- The Intel sweep runs only if the panel already has nits mode enabled (`0x344` bit 4, set by the firmware); the VESA
  sweep leaves its enable and mode bits as they are. Neither is changed.
- The lowest level is 10 % of the starting level, never zero. Starting levels below 20 are not scaled at all.
- The starting level is written back at the end, also after an error. If anything still looks wrong, shut the machine down
  completely (not only restart) and remove `-auxdimtest`.
- `-auxdimoff` disables the plugin entirely.

## How to run it

1. Install the kext (a release zip, or build it with `make`, see the [README](../README.md)), keep Lilu before it.
2. Add `-auxdimtest` to `boot-args` (keep `-auxdimdbg`; `-auxdimprobe` is implied). Optional: `-auxdimtestvesa`, `auxdimtestdelay=60`.
3. Boot to the desktop and look at the screen about two to three minutes after power on. Note roughly when it dims and
   what happens (dims in three steps / flicker / nothing).
4. Collect:

```bash
ioreg -rn AuxDim -l -w0 | grep -E '"(test-|intel-state|vesa-state|edid-lum|brightness-|hint-)' > auxdim-test.txt
ioreg -rn AuxDim -l -w0 > auxdim-ioreg.txt
```

The `test-*` lines contain the time since boot in seconds, to match with what you saw.

## Reading the result

```
test-armed          = done
test-intel          = finished, starting level restored
test-intel-step0    = uptime 143s: wrote 199 (50%) ret 0x0, read back 199 ret 0x0
test-intel-restore  = uptime 158s: wrote 399 ret 0x0, read back 399 ret 0x0
```

| Observation | Meaning |
|---|---|
| Screen dims during one sweep only | That interface is the backend |
| Dims in both | Both work, the Intel one is the natural choice (nits, same as the Linux driver and the reference kext) |
| Registers read back what was written, screen never changes | The panel accepts the value but something else holds the luminance (another control bit, or macOS rewrites it) |
| `ret` is not `0x0` | The framebuffer driver does not support native AUX writes in this transaction shape |
| Read back differs from what was written | The TCON clamps or ignores the value |
| `skipped: ...` | The interface was not advertised or not enabled, see `intel-state` and `vesa-state` |

Whatever the outcome, attach `auxdim-test.txt` and `auxdim-ioreg.txt` to a hardware report.

## What this test does not do

It does not apply the macOS brightness setting (that is Phase 1) and does not handle sleep and wake. After
`test-armed = done` the plugin does nothing in the background.

## Results so far

First run, Lenovo IdeaPad Slim 5 14IAH8, panel `ATNA40CT01-0`, Tahoe, WhateverXe, no SSDT-PNLF (the test does not need it):

- **AUX writes work.** The VESA sweep wrote 32767, 16383, 6553 and the starting 65535 into `0x722`/`0x723` and read each back,
  so a native AUX write through `IOFramebufferI2CInterface` is accepted and lands in the panel.
- **VESA does not dim this panel.** The screen did not change; its enable bit (`0x720` bit 0) is clear and the mode is 3
  (product). The register is only storage while the interface is not in use.
- **Intel 2 byte writes are ignored.** Writing 199, 99 and 39 to `0x354`/`0x355` returned success, but the register kept
  reading 399 and the screen did not change. The current build writes the 4 byte block the Linux driver writes.

Second run, same machine, 4 byte block:

- **Intel 4 byte writes dim the panel.** The screen dimmed in three steps (50%, 25%, 10%) and went back to the starting level.
  This is the backend; brightness control (Phase 1, [CONTROL.md](CONTROL.md)) uses it.
- **The panel reads back one nit below what is written** (199 reads 198, 99 reads 98, 39 reads 38, 399 reads 398). The firmware's
  own level was therefore most likely 400 nits, the frame-average luminance in the EDID. The test accepts a difference of one
  when it checks the restore.
