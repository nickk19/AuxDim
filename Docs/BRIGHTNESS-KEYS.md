# Brightness keys

The plugin applies whatever brightness macOS asks for ([CONTROL.md](CONTROL.md)). It does not see the keyboard: for the brightness
keys to work, macOS first has to receive them as brightness keys. On some laptops it does not, even though the slider works.

## Why the keys can do nothing

On many laptops the embedded controller (EC) raises a *query* when a brightness key is pressed, and the firmware answers it
with an ACPI notification meant for Windows:

```
Method (_Q11) { Notify (^^^GFX0.DD1F, 0x87) }   // ACPI "decrease brightness"
Method (_Q12) { Notify (^^^GFX0.DD1F, 0x86) }   // ACPI "increase brightness"
```

macOS ignores that notification. How to tell that this is your case: with the plugin running, press a brightness key and read
`brightness-events` (see below); if it does not change, the key never reached the display stack. If it does change but the panel
does not, that is a different problem, please report it.

## What the fix looks like

An SSDT replaces the query methods. Under macOS they notify the PS/2 keyboard (VoodooPS2Keyboard, an ACPI `Notify` to `PS2K`);
under any other OS they call the original methods, which two ACPI patches rename:

| Key | Notify value | Scan code | Mapped by VoodooPS2 to |
|---|---|---|---|
| down | `0x0405` | `e0 05` (make and break) | brightness down |
| up | `0x0406` | `e0 06` (make and break) | brightness up |

[Resources/ACPI](../Resources/ACPI) has an example, `SSDT-BRTKEYS` and `BRTKEYS-patches.plist`. **It is an example, not something to
copy blindly**: the query numbers and the ACPI paths are firmware specific, so they have to be adapted to your own DSDT. Some laptops
need nothing like this, some need it with other numbers, some route the keys elsewhere altogether.

## Adapting it

1. Dump your DSDT (from Linux: `cat /sys/firmware/acpi/tables/DSDT > DSDT.aml`, or with SSDTTime) and disassemble it: `iasl -d DSDT.aml`.
2. Look for the `_Qxx` methods that do `Notify (..., 0x86)` and `Notify (..., 0x87)`. Note their numbers and the path of the device that holds them (the EC).
3. Edit `SSDT-BRTKEYS.dsl`: the method names, the `External` lines and the `Scope`. Check the path of the keyboard device (`PS2K`) too.
4. Edit the two patches: `Find` is the method definition as it is in the binary, `14 <length> 5F 51 xx xx 00` (`_Qxx`), `Replace` is the same
   with `X` instead of `_`. Each must match exactly one place in the DSDT; count the matches before you rely on it.
5. Compile with `iasl SSDT-BRTKEYS.dsl`. To check it against your DSDT, apply the two renames to a copy and compile with `iasl -e <patched DSDT> SSDT-BRTKEYS.dsl`:
   it must report no errors.

## Installing

1. Copy the `.aml` into `EFI/OC/ACPI/` and add it under `ACPI -> Add` in `config.plist`.
2. Add the two patches under `ACPI -> Patch`.
3. Keep `SSDT-PNLF.aml` (the slider) and VoodooPS2Keyboard. Reboot.

Check: press the keys and read the counters.

```bash
ioreg -rn AuxDim -l -w0 | grep -E '"(brightness-events|brightness-last|ctl-last)'
```

`brightness-events` must go up and the panel must follow.

## If it does not work

- **`brightness-events` does not change**: the notification is not arriving. Check that the SSDT is listed and enabled under `ACPI -> Add`,
  that both patches are enabled, and that VoodooPS2Keyboard is loaded. The OpenCore debug log (Misc -> Debug) shows which ACPI tables and
  patches were applied.
- **The two keys are swapped**: exchange `0x0405` and `0x0406` in the SSDT and recompile.
- **Plug-ins that watch the keys (YogaSMC and the like)** work on other events; they do not see queries that end in a display notification.
