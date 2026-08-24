# programmer-app-gui

The GUI application that controls the Blue Pill over UART resides here.
Only Windows is supported. It uses plain Win32, no dependencies beyond
`comctl32` / `comdlg32`, so the application footprint is tiny.

## Build

```
mingw32-make          ->  ../bin/programmer_gui.exe
mingw32-make clean    remove ./build, keep the released binary
```

The protocol core is compiled from `../programmer-app-cli`, so that folder must
be present. Nothing else is required.

The executable is built straight into `../bin`, the compiled resources go to `./build`.

## Explanation of the folder content

| File | Purpose |
|---|---|
| `main_gui.c` | The whole interface: controls, tabs, worker thread, jobs. |
| `app.rc` | Resource script: the manifest and the twelve debugger icons. |
| `app.manifest` | Requests common controls v6 (themed widgets) and `asInvoker`. |
| `res.h` | Icon resource IDs, shared by `app.rc` and `main_gui.c`. |
| `icons/*.ico` | Toolbar icons, multi-size (16/24/32/48) 32-bit RGBA. |

Everything that talks to hardware lives in `../programmer-app-cli`; this file
only drives it. The CLI exposes the same operations, so neither program can do
something the other cannot.

## Explanation of GUI

**Target** - connect, probe, and three lists: identity (chip ID, flash JEDEC and
size, SRAM, SWire divider, first 16 flash bytes, boot marker), capabilities with
what this silicon does and does not expose, and the memory regions. Plus power,
halt, run and reset.

**Memory** - pick a region (Flash / SRAM / Registers), read to a hex dump or
straight to a file, or write hex bytes with automatic read-back verification.
Choosing Flash and trying a byte-wise write redirects you to the Flash tab
rather than failing quietly.

**Flash** - read a range, dump everything, erase sectors, restore the boot
marker, and program from a `.bin` with **file offset**, **length** and **target
address** so a portion of a file can be written. Separate Program and Verify
buttons. Destructive actions confirm first.

**Bridge** - self-test, pin test, power-rail selection, activation frame count
and the nine SWire timing parameters. This tab is about the bridge and the wire
rather than the target: it is where to go when nothing is communicating.

**Debugger** - A working debugger for the TLSR82xx: pause and resume, three
hardware breakpoints, single-step, step over, step out, run-to-cursor, the CPU
register file, a stack view, a disassembly listing read off the chip, and PC
sampling while the target runs.

### Power, then attach

The application starts with the target **not powered**. So you can read/write
flash even off of a target that had never ran.

The power button powers the target up **without activating it**, so it boots its
own firmware. Activation catches the chip in the post-reset window *before* the
firmware runs, and that state survives a soft reset, so an target activated can
never be debugged without being power-cycled again. You can however select any
debugger of flash function and the device will be powered automatically.

**Attach** (the leftmost button) then opens a session: it switches the bridge to
the live SWire divider, because the firmware sets a 48 MHz clock and never
rewrites `SWIRE_DIV`, and at the default rate every byte reads back shifted one
bit - which looks exactly like a dead link. If the chip turns out to be caught
at boot after all, debugger tries to recover.

**Stop** ends the session: comparators cleared, core running, bridge back on its
programming divider, target still powered. **Restart** reboots the core without
closing the session.

Closing the window detaches first, so nothing is left frozen or misclocked. If
the application is killed rather than closed, the next programmer operation
notices the debug divider and puts it back on its own.

### What each button does

| button | behaviour |
|---|---|
| Attach | open a session on the running firmware |
| Stop | end the session; the core is left running |
| Restart | reboot the core, keeping the session |
| Continue | run until a breakpoint is hit |
| Pause | stop the core where it is |
| Step into | execute the instruction at the PC; a call lands on the callee's first instruction |
| Step over | as Step into, but a call is run through and the core stops after it |
| To caller | run until this function returns, stopping just after the call that entered it |
| Step instruction | one instruction - the same thing as Step into here, see below |
| Run to cursor | run to the selected line in the listing, then stop |
| Toggle breakpoint | add or remove a breakpoint at the PC |
| Clear breakpoints | remove all of them |
| Refresh | re-read state, registers and stack without disturbing the core |

Step into and Step instruction coincide because there is no line-number
information: an instruction is the smallest step there is, so a "source step"
and a "machine step" are the same operation. Both exist for future compatibility.

Every action ends by re-reading `tlsr_dbg_status_read()`, and the buttons gray out
based on the capability bits it returns.

| state | what is enabled |
|---|---|
| power off | nothing |
| powered, detached | Attach, and nothing else |
| caught at boot | Attach - flash and memory work, but there is nothing to debug |
| attached, running | Pause, Stop, Restart, Refresh |
| attached, stopped | everything except Pause and Attach |

### Breakpoints

There are **three** breakpoints and the chip has four address comparators at `0x0610`.
Debugger keeps one for itself. (for step commands and orchestration). This breakpoint
also holds the core when needed.

A breakpoint reaches the chip the moment you set it. Writing a comparator does
not disturb a stopped core. A comparator also survives its own hit, so a
breakpoint stays live across any number of stops until you remove it.

Removing the breakpoint you are stopped *on* is the one case that needs care,
because clearing the comparator that is holding the core lets it go. The anchor
is moved onto that address first, so removing a breakpoint never starts the
program.

### Pause

Halt core command takes the bus from the MSPI controller, and the firmware
executes from flash, so a stall landing mid-fetch deadlocks the core. It can
be recovered only by a cold power cycle.

Instead it samples the live PC of the *running* core. Any sample is a
real fetch address, so arming a comparator there and waiting for execution to
come round again produces a stop that holds. It samples several dozen times and
arms the addresses it saw **most often**, across every spare comparator at once.
Picking merely-distinct addresses failed badly right after a boot, where the
firmware is running one-shot startup code.

Every stop is then watched for longer than the stall survives before it is
believed. Without that check a resume that never took looks exactly like an
instant comparator hit - the halted flag is still set, the PC has not moved -
and half a second later the core is gone while the register pane still shows
where it used to be.

### Stepping

Real hardware stepping, using Telink's own sequence: hold with bit1 of `0x0602`,
then pulse bit7 of `0x0613`, both read-modify-write. It follows branches and
calls, and needs no code image.

There is a special case debugger handles: A four-byte call takes two pulses, and
one pulse leaves the PC on the second halfword - a real machine state, but not an
instruction you are looking at, so it steps again. And this firmware services
interrupts constantly, so a step often lands on the IRQ vector; the instruction
*has* executed by then and LR holds exactly where the step was heading, so the
handler is run out to LR with a comparator. The log says how many interrupts
were passed through.

A few instructions will not retire under a single step at all, A breakpoint
on an interrupt handler's entry never fires while the core supposedly steps.
Those are run past with a comparator on the next address instead. An
instruction that branches to itself is reported as such rather than being run
into a timeout.

### The one real limitation

**Firmware with hard real-time work can lock up if the core is held too
often.** Every hold costs milliseconds, because SWire is slow, and a radio or a
protocol timer that misses its deadline does not always recover.Hammering pause
and resume in a tight loop breaks the debug process within a handful of rounds,
always ending stuck at one address, and a soft reset does not rescue it.

Debugger tries to recognize this state instead of reporting nonsense
("the firmware has stopped executing... Press Restart"), and
**Restart** verifies that the core is actually executing afterwards rather than
trusting `CLKSET`, falling back to a cold power cycle when the soft reset does
not take.

Keeping the traffic per operation down is what makes ordinary use comfortable:
the step path reads its comparator state from a software mirror rather than
re-reading the block, and dropping those four SWire reads per step took the
acceptance suite from locking the target up on most runs to four clean runs in a
row.

### Disassembly

Appears as soon as the core is stopped. There are three columns: 1. breakpoint marker,
2. address, 3. symbol or instruction. The current PC is highlighted and also
marked `>` in the address column - the marker matters because Windows paints its
own selection colour over custom-drawn rows, so colour alone cannot be relied
on. Double-clicking a line toggles a breakpoint there.

The code comes off the **target**, not from a file: flash can be read while the
core is stopped, provided the read does not issue its own `0x05` stop, which
would end the session. It is fetched in 512-byte pieces, because larger reads
corrupt at the doubled SWire divider - 1 KB came back with 238 wrong bytes and
knocked the core loose.

**Symbols.** Load a symbol table with **Symbols...**, or pass `-s <file>` on the
command line (`-c <file>` for a code image). `tc32-elf-nm` output and plain
`address name` lines are both accepted. A symbol gets its own header row above
its first instruction, the way an objdump listing reads:

```
0000140C   <sub_0140c>:
>  00140C     tmovs r2, #0
```

The status line also names the PC, as `<function+0x...>`.

**Scrolling.** The list is a virtual list, so rows are produced only as they are
shown and the decoded span can grow at either end without the control being
rebuilt. Reaching either end quietly fetches more; when code is added *above*
what you were reading, the view is scrolled down by exactly the number of rows
inserted, so nothing moves under the cursor.

**Code...** loads a host-side image. It is optional now - instructions are read
off the chip - and only saves a SWire round trip per decode.

## Implementation notes
The bridge is not thread safe, so every operation runs on **one worker thread,
one at a time**. The worker never touches a window - it appends to a log buffer
and updates a progress counter under a critical section, and a 100 ms timer on
the UI thread moves both into the controls. That is why a 512 KB dump (~25 s)
neither freezes the window nor looks like it has hung: long operations announce
themselves, report progress every couple of seconds with a percentage, and print
their elapsed time.

Job parameters are copied out of the controls when the job is queued, so editing
a field mid-operation cannot change what is already running.

*The log is an `ES_READONLY` edit control.* `EM_REPLACESEL` is silently ignored
on a read-only control, so appending clears the flag, appends, and sets it back.
Without that the log stays permanently empty - which is exactly what happened
during development.

*Controls are shown and hidden per tab* rather than living on child dialogs. It
keeps everything in one window procedure and avoids a dialog resource, at the
cost of one list of control IDs per tab.

*Page controls are siblings of the tab control, and `CreateWindowEx` inserts
each new child at the **bottom** of the z-order* - so everything created after
the tab control sat behind it and every tab looked blank. One
`SetWindowPos(g_tabs, HWND_BOTTOM, ...)` after the controls are built fixes it.

*A control created with id 0 is unreachable.* `GetDlgItem` cannot find it, so
`show_ids()` can never reveal it and it stays invisible for the life of the
window. Every static label therefore has a real ID.
