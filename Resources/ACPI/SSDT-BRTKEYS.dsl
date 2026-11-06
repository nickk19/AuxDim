/*
 * SSDT-BRTKEYS: example of getting the brightness keys to macOS on a laptop whose keys never arrive as brightness keys.
 *
 * On many laptops the embedded controller (EC) raises a query when a brightness key is pressed, and the firmware answers it
 * with an ACPI notification meant for Windows, for instance
 *
 *     Method (_Q11) { Notify (\_SB.PC00.GFX0.DD1F, 0x87) }   // ACPI "decrease brightness"
 *     Method (_Q12) { Notify (\_SB.PC00.GFX0.DD1F, 0x86) }   // ACPI "increase brightness"
 *
 * macOS ignores that notification, so the keys do nothing. This SSDT replaces such query methods: under macOS they
 * notify the PS/2 keyboard (VoodooPS2Keyboard) with the scan codes it maps to the brightness keys, under any other OS they
 * call the original methods, which two ACPI patches (BRTKEYS-patches.plist) have renamed to XQ11 and XQ12.
 *
 *     Notify (PS2K, 0x0405)  ->  scan code e0 05, make and break  ->  brightness down
 *     Notify (PS2K, 0x0406)  ->  scan code e0 06, make and break  ->  brightness up
 *
 * (Notify value: high byte 3 or 4 = send make and break, low byte = scan code, 4 = extended e0 prefix.)
 *
 * The query numbers (_Q11, _Q12) and the ACPI paths are firmware specific: they are the ones of the DSDT this example was
 * written against. Find yours in your own DSDT and adapt this file and the patches, see Docs/BRIGHTNESS-KEYS.md.
 * Needs VoodooPS2Keyboard. Compile: iasl SSDT-BRTKEYS.dsl
 */
DefinitionBlock ("", "SSDT", 2, "AUXDIM", "BRTKEYS", 0x00000000)
{
    External (_SB_.PC00.LPCB.EC0_, DeviceObj)
    External (_SB_.PC00.LPCB.EC0_.XQ11, MethodObj)
    External (_SB_.PC00.LPCB.EC0_.XQ12, MethodObj)
    External (_SB_.PC00.LPCB.PS2K, DeviceObj)

    Scope (\_SB.PC00.LPCB.EC0)
    {
        Method (_Q11, 0, NotSerialized)  // F5: brightness down
        {
            If (_OSI ("Darwin"))
            {
                Notify (\_SB.PC00.LPCB.PS2K, 0x0405)
            }
            Else
            {
                XQ11 ()
            }
        }

        Method (_Q12, 0, NotSerialized)  // F6: brightness up
        {
            If (_OSI ("Darwin"))
            {
                Notify (\_SB.PC00.LPCB.PS2K, 0x0406)
            }
            Else
            {
                XQ12 ()
            }
        }
    }
}
