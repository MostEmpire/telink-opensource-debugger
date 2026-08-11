# bin

Prebuilt binaries, so a fresh machine needs no toolchain.

| File | What it is |
|---|---|
| `blue-pill-firmware.hex` | Bridge firmware for ST-Link upload. **Use this one** — Intel HEX carries its own load address. |
| `blue-pill-firmware.bin` | Same image, raw. If your tool wants a `.bin`, load it at `0x08000000`. |
| `programmer_cli.exe` | CLI-based Telink programmer (64-bit Windows). |
| `programmer_gui.exe` | GUI-based Telink programmer (64-bit Windows). |

Built with MinGW-w64 gcc and `arm-none-eabi-gcc`. Both `.exe` files are
standalone: they use only the Win32 API, so there is no runtime to install.

Order of operations:

1. Program `blue-pill-firmware.hex` into the Blue Pill with an ST-Link.
2. Plug in the Blue Pill's own USB socket; it appears as a COM port.
3. If no COM-port is recognized when Blue Pill is plugged in, install ../driver
4. Run `programmer_cli.exe --list` to find the port, then `--info`, or use GUI.

To rebuild any of these from source, see the README in the matching source
folder. Both application Makefiles build **into this folder**.

`mingw32-make clean` in a source folder removes only that folder's `build/`
directory and leaves these binaries alone.