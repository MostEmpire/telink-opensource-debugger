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

These drive the CPU debug block. The firmware has to be **running**. Activation
catches the chip before it boots, and that state survives a soft reset.
So power the target *without* activating it, then open a session. The target
is left running between invocations, so a session spans several commands.

| Action | Meaning |
|---|---|
| `--dbg-power-on` | power the target, no activation, so the firmware boots |
| `--dbg-power-off` | cut the rails |
| `--dbg-attach` | open a session on the running firmware |
| `--dbg-detach` | close it: comparators cleared, core running, bridge back on its programming divider |
| `--dbg-reset` | reboot the core without closing the session |
| `--dbg-status` | the target's state and what that state allows |
| `--regs` | dump the CPU register file |
| `--halt-core` | stop the core where it is |
| `--resume` | run on, waiting for a breakpoint if any are set |
| `--bp ADDR` | add or remove a breakpoint at ADDR |
| `--bp list\|off` | list them, or remove them all |
| `--runto ADDR` | run until the PC reaches ADDR, then stop (`--timeout`) |
| `--step [N]` | execute N instructions, default 1 |
| `--step-into` | one instruction, entering any call |
| `--step-over` | one instruction, running any call to completion |
| `--step-out` | run to the address in LR, i.e. finish this function |
| `--stack [N]` | dump N bytes of stack at SP, default 64 |
| `--profile [N]` | sample the PC N times while running, default 200 |

Attach sets the blue pill to live SWire divider -- the firmware runs at
48 MHz and never rewrites `SWIRE_DIV`, so at the default rate every byte comes
back shifted one bit and the link looks dead. If the chip turns out to be caught
at boot, debugger tries to recover.

An abandoned session would leave every later programmer command reading shifted
bytes and reporting a confident `0xFF62` chip id, so `tlsr_ensure_target()` puts
the divider back on its own before activating.

#### Three breakpoints

The chip has four address comparators at `0x0610`. The debugger keeps one for
itself (for step commands and orchestration). This breakpoint also holds the core
when needed.

A breakpoint reaches the chip the moment it is set -- writing a comparator does
not disturb a stopped core. A comparator survives its own hit, so a breakpoint
stays live until it is removed. Removing the one the core is stopped *on* would
let the core go, so the orchestration breakpoint is moved onto that address first.

#### `--halt-core` never stalls

Halt core command takes the bus from the MSPI controller, and the firmware
executes from flash, so a stall landing mid-fetch deadlocks the core. It can
be recovered only by a cold power cycle.

Instead the PC of the *running* core is sampled from `0x06BC`. Any sample is a
real fetch address, so arming a comparator there and waiting for execution to
come round again gives a stop that holds. It samples several dozen times and
arms the addresses seen **most often** across every spare comparator at once.
The stop is then watched for longer than the stall survives before it is believed,
because a resume that never took looks exactly like an instant hit.

#### Stepping

Real hardware stepping, using Telink's own sequence:

| op | `0x0602` | `0x0613` |
|---|---|---|
| stall | `= 0x06` | -- |
| step | `\|= 0x02` (hold) | `\|= 0x80` (go pulse) |
| run | `&= ~0x02` | `\|= 0x80` |

Both writes are read-modify-write, and the `0x0602` write is what arms the
pulse -- writing `0x0613` alone does nothing. The PC comes from `0x06BC`.

Three things the raw step needs help with:

- **A four-byte call takes two pulses.** One pulse leaves the PC on the second
  halfword, so `--step-into` on `tjl` would land two bytes on instead of in the
  callee. It steps again.
- **Interrupts.** This firmware services them constantly, so a step often lands
  on the IRQ vector `0x10`. The instruction *has* executed by then and LR holds
  exactly where the step was heading, so the handler is run out to LR with a
  comparator.
- **Instructions that will not retire.** A breakpoint on the interrupt handler's
  entry may never fire while the core supposedly steps. It may not move rather
  than quietly run a lap. Those are run past with a comparator on the next address.
  A self-branch is reported as one rather than run into a timeout.

`--code` is optional: instructions are read straight off the chip, because flash
can be read while the core is stopped as long as the read does not issue its own
`0x05` stop.

#### The one real limitation

**Firmware with hard real-time work can lock up if the core is held too
often.** Every hold costs milliseconds, because SWire is slow, and a radio or a
protocol timer that misses its deadline does not always recover.
No debugger can avoid that. What this one does about it: it recognises the state
and says so ("the firmware has stopped executing... Press Restart") instead of
reporting nonsense, and `--dbg-reset` checks that the core is actually executing
afterwards rather than trusting `CLKSET`, falling back to a cold power cycle
when the soft reset does not take.

Keeping the traffic per operation down is what makes ordinary use comfortable.
The step path reads its comparator state from a software mirror rather than
re-reading the block; dropping those four SWire reads per step took the
acceptance suite (`recon/tools/swireprobe/dbgacc.c`, 57 checks) from locking the
target up on most runs to four clean runs in a row.

#### A caveat specific to the CLI

Each command is its own process, and closing the serial port resets the bridge.
`--resume` waits for a breakpoint when any are set, rather than resuming and
exiting. If it times out it says the core is still running and that closing the
port now may clear the breakpoints.

Because `0x0602` reads back `0x08` no matter what is written to it, the tool
never trusts it for state. Running-versus-stopped comes from **`0x0613` bit 7**,
a genuine halted flag: 0 free-running, 1 after a stall, 0 after a resume, 1 at a
comparator stop. That is one byte where an earlier version sampled the PC ten
times over a quarter of a second.

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

programmer_cli -p COM10 --dbg-attach
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
