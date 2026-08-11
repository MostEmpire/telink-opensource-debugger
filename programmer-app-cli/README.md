# programmer-app-cli

The cli application that controls the Blue Pill over UART resides here.
Its protocol core is shared with the GUI counterpart.

## Build

```
mingw32-make          ->  ../bin/programmer_cli.exe
mingw32-make clean    remove ./build, keep the released binary
```

MinGW-w64 and the Win32 API only; nothing to install.

The executable is linked straight into `../bin`. Objects go to `./build`.

## Explanation of the folder content

| File | Purpose |
|---|---|
| `tlsr_core.h` | Public API and every protocol constant. Start here. |
| `tlsr_internal.h` | Shared between the core `.c` files; not part of the API. |
| `tlsr_serial.c` | Win32 handle on `\\.\COMx`, plus COM port enumeration. |
| `tlsr_bridge.c` | Command framing, SWire register/SRAM access, core control. |
| `tlsr_flash.c` | SPI-NOR flash through the target's MSPI controller, and identification. |
| `tlsr_debug.h` | CPU debug block: register file, single step, breakpoint|
| `tlsr_debug.c` | Implementation of the above. |
| `main_cli.c` | Argument parsing and the actions below. |

The GUI compiles every `tlsr_*.c` here straight out of this directory, so a fix
to the protocol, the flash logic or the debug block reaches both programs. The
two front ends expose the same feature set.

Every function returns `TLSR_OK` or a negative `TLSR_E*` code and leaves a
human-readable explanation in `tlsr_last_error()`.

## Usage

```
programmer_cli -p PORT ACTION [OPTIONS]
```

### Actions

| Action | Meaning |
|---|---|
| `--list` | list COM ports and exit (no `-p` needed) |
| `--info` | identify bridge and target, print capabilities |
| `--dump` | read flash to a file (`-f`) |
| `--flash` | erase + program + verify a file into flash (`-f`) |
| `--verify` | compare flash against a file (`-f`) |
| `--erase` | erase flash sectors (`-a`, `-n`) |
| `--read` | hex dump memory or flash (`-a`, `-n`, `--region`) |
| `--write HEX` | write bytes to memory (`-a`), e.g. `--write "A5 5A"` |
| `--halt` / `--run` / `--reset` | core control |
| `--power on\|off` | switch target power |
| `--activate` | re-open the SWire link without touching anything else |
| `--selftest` | bridge loopback check, needs no target |
| `--pintest` | PA6/PA7 continuity check through the 750 Ω |
| `--cfg` | print the bridge's SWire timing |
| `--set-cfg K=V,..` | change it, e.g. `--set-cfg spi_div=2,cell=6` |

### Debugger actions

These drive the CPU debug block. **TLSR8253 semms not to have the debug block accessible** - `0x0680` and `0x06BC` read back zero with the core held, and
`0x0602 <- 0x06` does not stall. They are here because the register map is
right for the family and the chip may differ on another board; run
`--dbg-probe` first.

| Action | Meaning |
|---|---|
| `--dbg-probe` | does the debug block answer at all? exit 0 = yes |
| `--regs` | dump the CPU register file |
| `--stall` | debug-halt the core, then read `0x0602` back and say whether it took |
| `--continue` | resume with the breakpoint comparator live |
| `--step [N]` | execute N instructions, default 1 |
| `--step-over` | step, but run through a call rather than into it |
| `--step-out` | run to the address in LR, i.e. finish this function |
| `--bp ADDR\|off` | arm or clear the hardware breakpoint |
| `--runto ADDR` | resume and wait for the PC to reach ADDR (`--timeout`) |

### Options

| Option | Meaning |
|---|---|
| `-p, --port PORT` | bridge COM port, e.g. `COM10` |
| `-f, --file PATH` | binary file |
| `-a, --addr ADDR` | start address, decimal or `0x` hex (default 0) |
| `-n, --length N` | byte count (default 256) |
| `--offset N` | start N bytes into the file, for `--flash` / `--verify` |
| `--region NAME` | `flash` (default), `sram` or `reg`, for `--read` / `--write` |
| `--rail both\|pb0\|mosfet` | which supply to switch (default `both`) |
| `--timeout MS` | how long `--runto`, `--step-over` and `--step-out` wait (default 5000) |
| `--frames N` | activation frames for `--activate` (default 600) |
| `-q, --quiet` | only results and errors |

Addresses and lengths accept `0x` hex or decimal everywhere.

## Examples

```
programmer_cli -p COM10 --info
programmer_cli -p COM10 --dump  -f backup.bin
programmer_cli -p COM10 --flash -f repaired.bin
programmer_cli -p COM10 --verify -f repaired.bin

# program only part of a file, to a matching offset in flash
programmer_cli -p COM10 --flash -f image.bin --offset 0x1000 -n 0x1000 -a 0x1000

programmer_cli -p COM10 --read -a 0x040000 -n 64 --region sram
programmer_cli -p COM10 --write "DE AD BE EF" -a 0x040000 --region sram

programmer_cli -p COM10 --dbg-probe
programmer_cli -p COM10 --cfg
```

## Behaviour worth knowing

**Power and activation are automatic.** Any action that touches the chip powers
it up and activates it first, so there is no ordering to remember. Activation
is a burst of frames aimed at the ~8 ms window after power-on during which the
target's SWire block starts answering.

**Programming preserves the rest of the sector.** Flashing touches whole 4 KB blocks,
so if you wish to flash only a part of sector, `--flash` reads the sector, modifies what
is necessary and writes everything back, keeping the rest of the sector intact.

**Flash cannot be written byte-wise.** `--write` deliberately refuses the flash
region and points at `--flash`, rather than appearing to work and silently
failing on un-erased bits.

**Take a backup first.** `--dump` a full image before any write. A 512 KB dump
takes about 25 seconds.

**SWire reads are only trustworthy with the core stopped.** With it running,
bytes come back bit-shifted - the `0x007E` chip id itself does. Every debug
action therefore holds the core first, and `--run` is worth thinking twice
about: once the firmware on the chip boots it may reconfigure the SWS pad as GPIO, and
getting back in needs a power cycle plus `--activate`.

## Exit status

`0` on success, `1` on failure, with the reason on stderr.
