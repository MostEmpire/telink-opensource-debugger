# driver

STMicroelectronics USB CDC (virtual COM port) driver.

When you flash Blue Pill and connect it over the USB, it advertises as VID `0483` / PID `5740`. **Windows 10 and 11 should bind their in-box `usbser.sys` driver automatically**. If however you plug Blue Pill to your computer and an unknown device is detected, installing this driver should allow Windows to recognize it as a COM serial device.

## Installing

Run the installer matching your Windows build:

```
dpinst_amd64.exe        64-bit
dpinst_x86.exe          32-bit
w8_install.bat          Windows 8 helper
```

`stmcdc.inf` and `stmcdc.cat` are the driver package itself.

## Provenance and licence

These files are STMicroelectronics' redistributable VCP driver, as shipped with
pvvx's Telink programmer package. They are **not** covered by this repository's
GPL-2.0 licence and are not our work — they are included only so a fresh
machine has everything in one place. ST's own licence terms apply.

If you would rather not use a bundled binary, the same driver is available from
STMicroelectronics as **STSW-STM32102**.
