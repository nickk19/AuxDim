# Phase 0 probing guide

The probe answers four questions about your machine, none of which can be guessed reliably from a model name:

1. Does macOS send brightness requests to your display at all?
2. Does your graphics driver expose a DisplayPort AUX (native) interface for the panel?
3. Which backlight interface does the panel implement: VESA eDP AUX, Intel HDR/SDR TCON nits, or only PWM?
4. What are the display's identity (EDID) and the range macOS uses for brightness?

The probe only **reads**. It never writes to the panel or to any register. The only part of the plugin that writes is brightness
control (a backend argument such as `-auxdimintel`, see [CONTROL.md](CONTROL.md)), and it writes only the registers of its backend.

## Steps

1. Add the kext after Lilu in OpenCore (`Kernel -> Add`), with the same architecture/min/max kernel settings you use for
   other Lilu plugins. A Debug build is preferable for the first run (extra log lines), a Release build also works. Both are in the
   release zips, or build one with `make`.
2. Add `-auxdimprobe` to `boot-args` (add `-auxdimdbg` too with a Debug build).
3. Boot, log in, then change the brightness once with the slider or the keys. The probe fires about three seconds after the
   first display parameter is seen, so if there is no slider at all just wait ten seconds: that case is a finding too.
4. Collect, in a terminal:

```bash
ioreg -rn AuxDim -l -w0 > auxdim-ioreg.txt                  # the probe results (main thing to attach)
kmutil showloaded 2>/dev/null | grep -i auxdim > kext.txt           # proves the kext is loaded
ioreg -rc IODisplay -l -w0 > displays.txt                          # all displays and their parameters
ioreg -rc IOFramebufferI2CInterface -l -w0 > i2c.txt               # AUX/I2C interfaces published by the driver
ioreg -l -w0 | grep -i -E "PNLF|APP0002|backlight" | cut -c1-200 > backlight-nodes.txt
sudo log show --last boot --info --debug --predicate 'eventMessage CONTAINS[c] "auxdim"' > auxdim-log.txt
```

   Every probe result is also a property of `AuxDim`, so the kernel log is optional: whether the plugin's lines reach
   `dmesg` and `log show` depends on the setup (on the development machine they did not), so an empty `auxdim-log.txt`
   is not a problem. Lilu can also collect them: with a Debug build of Lilu and `liludump=60` in `boot-args`, sixty
   seconds after boot it writes everything the plugins logged to `/var/log/Lilu_<version>_<darwin>.txt`. The Debug
   build of this plugin does not need a Debug Lilu for its extra lines (`-auxdimdbg`); only `liludump` does.

5. Open an issue titled *Hardware report: your laptop model* and attach the files (the EDID is not sensitive: it contains the model and a serial
   of the panel, remove the serial block if you prefer).

## Reading the output

Properties on `AuxDim`:

| Key | Meaning |
|---|---|
| `display-class`, `display-name` | Kernel class of the display object (`AppleBacklightDisplay` means macOS treats it as a built-in panel with brightness) |
| `edid`, `edid-block-N`, `edid-luminance` | Manufacturer, product, name (monitor name or, for panels that only carry a model code, the data string) and raw EDID blocks; peak and average luminance declared by the HDR static metadata block |
| `param-brightness`, `param-linear-brightness` | Range and current value macOS uses; `absent` means no slider for this display |
| `framebuffer-class` | The driver class owning the panel |
| `aux-interface-N` | One line per IOI2C interface. Bit 4 of `transaction-types` (`0x10`) is the native AUX transaction, `native-aux=yes` is what the plugin needs. `bus-type` is informative only (Apple's Intel framebuffer reports `1`, plain I2C, even for DisplayPort) |
| `dpcd-0000` ... `dpcd-0720` | Raw DPCD bytes, see below |
| `intel-state`, `vesa-state`, `amd-state` | The backlight interfaces decoded: capability bits, mode, enable state and the level currently programmed (AMD: in millinits, with the level the panel reports) |
| `source-oui` | The source OUI at `0x300`, which tells which GPU driver last wrote it (`AMD`, `Intel`, or `none written`) |
| `hint-backlight` | One line summary of the interfaces; the raw bytes are authoritative |
| `brightness-events`, `brightness-last` | How many brightness requests reached the hook and the last values, with uptime. Needs the display to have a brightness parameter (SSDT-PNLF) |

DPCD ranges read (hex bytes, first byte at the listed address):

| Address | Content |
|---|---|
| `0x000` | DPCD revision, max link rate, max lane count, ... |
| `0x300` | Source OUI last written by the GPU driver (`0x300`-`0x302`), then AMD's backlight registers: panel caps (`0x317`), level in millinits and transition (`0x320`), current peak and average (`0x326`, `0x32A`), control (`0x32E`), enable (`0x32F`) |
| `0x340` | Intel HDR/SDR TCON capabilities and control (`0x344`), nits brightness (`0x354`) |
| `0x400` | Sink OUI and device identification string (panel TCON vendor) |
| `0x700` | eDP revision (`0x700`), general capability (`0x701`), backlight adjustment capability (`0x702`) |
| `0x720` | eDP display control (`0x720`), backlight mode (`0x721`), brightness MSB/LSB (`0x722`/`0x723`), PWM generator |

How to read the result:

- `aux-interface-count` is `0`, or no line has `native-aux=yes`: the driver does not expose AUX through the standard interface;
  the AUX backend needs another route (this is the main open risk, see the architecture document).
- `vesa-aux-set:yes` means the panel accepts brightness through DPCD `0x722`/`0x723`.
- `intel-hdr-nits:yes` / `intel-sdr-aux:yes` means the Intel TCON nits interface (`0x354`) is present.
- Neither of the above: the panel is probably PWM only, and the normal GPU backlight path is what you need.

## Optional cross-check from Linux

If you can boot a Linux live image on the same machine, the kernel can expose the same bytes through the DisplayPort AUX
character device (when the kernel is built with `CONFIG_DRM_DP_AUX_CHARDEV`):

```bash
sudo dd if=/dev/drm_dp_aux0 bs=1 skip=$((0x700)) count=48 2>/dev/null | xxd
```

Which `drm_dp_aux` index belongs to the panel varies; check `dmesg | grep -i aux` and `ls /dev/drm_dp_aux*`.
