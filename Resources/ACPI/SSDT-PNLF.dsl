/*
 * SSDT-PNLF: publishes the ACPI "backlight" device that macOS looks for.
 *
 * Without it, IOGraphics treats the internal panel as a plain AppleDisplay: no brightness parameter,
 * no slider, no brightness keys. With it the panel becomes an AppleBacklightDisplay and every
 * brightness request passes through IODisplay::doIntegerSet, which is where AuxDim listens.
 *
 * This SSDT is generic: it touches no GPU register and no PCI device, so it does not depend on the
 * graphics stack. Do NOT add it if your DSDT or another SSDT already defines a PNLF device
 * (check with: ioreg -l | grep -i -E "PNLF|backlight").
 *
 * _UID is read by WhateverGreen, not by macOS or AuxDim: on Intel graphics it picks the PWM range that
 * WhateverGreen programs for the framebuffer's backlight. 0x13 (19) is its profile for Coffee Lake and newer,
 * a 16 bit range (maximum 0xFFFF), which is what the Intel framebuffer reports for this panel (bklt max 65535).
 * AuxDim takes the range from the brightness parameter itself, so if your setup already uses another value,
 * keep it.
 *
 * Compile: iasl SSDT-PNLF.dsl   (iasl: `brew install acpica`, or the ACPICA release binaries)
 * Use:     copy SSDT-PNLF.aml to EFI/OC/ACPI/ and add it in config.plist -> ACPI -> Add.
 */
DefinitionBlock ("", "SSDT", 2, "AUXDIM", "PNLF", 0x00000000)
{
    Scope (\_SB)
    {
        Device (PNLF)
        {
            Name (_HID, EisaId ("APP0002"))  // _HID: Hardware ID
            Name (_CID, "backlight")         // _CID: Compatible ID
            Name (_UID, 0x13)                // _UID: Unique ID
            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                If (_OSI ("Darwin"))
                {
                    Return (0x0B)
                }
                Else
                {
                    Return (Zero)
                }
            }
        }
    }
}
