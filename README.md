# AuxDim

GPU-agnostic brightness control for displays that macOS cannot dim through the usual PWM backlight path:
OLED eDP panels dimmed over the DisplayPort AUX channel, and (later) external monitors dimmed over DDC/CI.
It is a [Lilu](https://github.com/acidanthera/Lilu) plugin for Hackintosh.

**Status: Phase 1, opt-in, tested on two machines (one Intel, one AMD).** Brightness control goes through a
backend, chosen with its boot argument: `-auxdimintel` applies the brightness macOS asks for through the Intel nits interface (DPCD
`0x354`), which is what the Lenovo IdeaPad Slim 5 14IAH8 panel `ATNA40CT01-0` turned out to need; `-auxdimamd` does the same
through AMD's backlight interface, the one the Linux amdgpu driver uses for OLED panels (tested on an ASUS UM325U with NootedRed). With `-auxdimprobe` instead
the plugin only reads the panel for diagnostics; with neither it does nothing. Setup and what to report: [Docs/CONTROL.md](Docs/CONTROL.md).
See [Docs/ARCHITECTURE.md](Docs/ARCHITECTURE.md) for the design and the roadmap.

## Why another brightness kext

The existing OLED brightness kext for macOS
([gil-666/DELL-XPS-OLEDBrightness](https://github.com/gil-666/DELL-XPS-OLEDBrightness)) proves the idea works, but it is
tied to one GPU (PCI `8086:3E9B`), pokes the GPU's AUX registers directly and depends on WhateverGreen writing the
PWM duty register. This project instead sits above the graphics driver (`IODisplay`) and talks to the panel through the
driver's own AUX/I2C interface, so the same plugin can work with Intel, AMD or NVIDIA stacks and with laptops or monitors.
Details and caveats are in the architecture document.

## Requirements

- Lilu 1.7.1 or newer, loaded before this plugin (1.7.1 is the first release that loads on macOS 26 without `-lilubetaall`);
  built and tested with 1.7.2
- macOS Catalina to Tahoe is the range the plugin accepts; it has only been tested on Tahoe
- For brightness control the internal panel must show up with a brightness slider in macOS (on a Hackintosh that normally means an
  `SSDT-PNLF`, a generic one is in [Resources/ACPI](Resources/ACPI)); this plugin does not create the slider. Not needed for the probe

## Boot arguments

| Argument | Effect |
|---|---|
| `-auxdimoff` | Disable the plugin |
| `-auxdimdbg` | Verbose logging (effective in Debug builds) |
| `-auxdimbeta` | Allow running on macOS newer than the tested range |
| `-auxdimprobe` | Run the read-only hardware probe, see [Docs/PROBING.md](Docs/PROBING.md) |
| `-auxdimintel` | Backend: applies the macOS brightness through the Intel nits interface (implies the probe), see [Docs/CONTROL.md](Docs/CONTROL.md) |
| `-auxdimamd` | Backend: the same through AMD's backlight interface (DPCD `0x320`), as amdgpu drives OLED panels; it switches that interface on itself (`0x32F`) when it is off |
| `auxdimnits=N` | Sets this luminance (nits) as soon as the controller is up, until macOS sends a brightness |
| `auxdimminnits=N` | Lowest luminance ever sent, in nits. By default 0: at the lowest slider position the panel goes dark, as on a Mac; set it to keep the lowest notch visible |
| `auxdimmaxnits=N` | Luminance at full brightness (100 to 1000 nits) for panels whose EDID does not give it; without it such a panel is left alone |

Give exactly one backend argument: with more than one, control stays off and `ctl-state` says so. `auxdimnits`, `auxdimminnits`
and `auxdimmaxnits` only apply together with a backend.

## Install

1. Download the latest release from the [Releases](../../releases) page and unzip it. There are two zips: `AuxDim-<version>-RELEASE.zip`
   for everyday use and `AuxDim-<version>-DEBUG.zip`, which adds the verbose log lines enabled by `-auxdimdbg` (use it when you report
   a problem). Prefer to compile it yourself? See *Building from source* below.
2. Copy `AuxDim.kext` into `EFI/OC/Kexts/` and add it to `config.plist` under `Kernel -> Add`, **after Lilu**.
3. For the slider, add `SSDT-PNLF.aml` from [Resources/ACPI](Resources/ACPI) (details and checks in [Docs/CONTROL.md](Docs/CONTROL.md)).
4. Add the backend argument (`-auxdimintel`, or `-auxdimamd` on AMD) to `boot-args` and reboot. The slider in System Settings > Displays should now move the panel.

Brightness control is described in [Docs/CONTROL.md](Docs/CONTROL.md). To find out what your panel supports before enabling it,
run the diagnostics (phase 0):

1. Install the Debug kext as above.
2. Boot with `-auxdimprobe`, change the brightness once, wait a few seconds.
3. Collect the output as described in [Docs/PROBING.md](Docs/PROBING.md) and open an issue titled *Hardware report: your laptop model*.

## Building from source

Only the **Xcode Command Line Tools** are needed (`xcode-select --install`): clang, the linker, `make`, `git`, `curl`,
`unzip`, `zip`. Xcode itself is not required and `xcodebuild` is never called.

```bash
./bootstrap.sh                # once: fetches MacKernelSDK and the Lilu SDK into Deps/
make doctor                   # optional: shows the toolchain and SDKs it found
make                          # Debug build   -> build/Debug/AuxDim.kext
make CONFIG=Release           # Release build -> build/Release/AuxDim.kext
make dist                     # Debug and Release zips, ready to attach to a release
make clean                    # remove build/ (the fetched SDKs in Deps/ stay)
make distclean                # also remove the fetched SDKs (Deps/)
./bootstrap.sh --clean        # remove only the fetched SDKs
```

Fetched dependencies live in `Deps/`, everything compiled in `build/` (including the Lilu code the plugin
links: `plugin_start.cpp` is compiled to `build/<Configuration>/obj/`).

- The SDKs are pinned: MacKernelSDK to a fixed commit and Lilu to 1.7.2, whose archive is checked against its SHA-256.
  To build against the Lilu release you actually boot with: `LILU_VERSION=1.7.1 ./bootstrap.sh --force` (`--force`
  replaces SDKs that are already there; other releases are not checksummed, the script says so).
- `make V=1` prints every command. If the final link step fails, send that output in an issue. The classic linker
  is worth a try: `make LD_CLASSIC=1`.
- `make check` only compiles (no link) with the same flags; it also works with a recent clang on Linux.
- After the link the Makefile inspects the binary and prints a `WARNING` if it is not a kext bundle or references
  user space symbols (which would stop it from loading).

The kext is not signed. It is meant to be injected by OpenCore, which does not need a signature.

## Layout

```
Makefile                   command line build, no Xcode project
bootstrap.sh               fetches (and removes) the build dependencies into Deps/
AuxDim/                    plugin sources
  kern_start.cpp           Lilu entry point and boot arguments
  kern_auxdim.{hpp,cpp}    IODisplay hook, deferred probe scheduling
  kern_dpaux.{hpp,cpp}     DisplayPort AUX access through the driver's IOI2CInterface
  kern_probe.{hpp,cpp}     read-only hardware probe, property publishing
  kern_edid.{hpp,cpp}      EDID name and HDR luminance
  kern_backlight.{hpp,cpp} the two DPCD backlight interfaces; the only write gate (level registers only)
  kern_policy.hpp          brightness value -> nits mapping (pure integer maths, unit tested on a host)
  kern_control.{hpp,cpp}   brightness control: records requests in the hook, applies them from a thread call
  Info.plist.in            bundle Info.plist template (filled in by the Makefile)
  kmod_info.c.in           kernel module descriptor template (what Xcode would generate)
Docs/                      architecture, brightness control, brightness keys, probing guide
Resources/ACPI/            SSDT-PNLF (the slider); SSDT-BRTKEYS with its ACPI patches (example: brightness keys through VoodooPS2); source and compiled
Deps/                      MacKernelSDK and Lilu SDK, fetched by bootstrap (not tracked)
```

## License

BSD 3-Clause, see [LICENSE](LICENSE). Acknowledgements and third-party notices are in [NOTICE.md](NOTICE.md).
