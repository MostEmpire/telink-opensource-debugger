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

**Debugger** - The application contains a control surface for the complete debugging
functionality, although it is very experimental and does not work on the tested TLSR8253 chip.
The application automatically grays out the buttons that cannot be used.
On TLSR8253 the CPU debug block does not answer: `0x0680` and `0x06BC` read
back zero with the core held, and `0x0602 <- 0x06` does not stall. Anything
needing a readable PC - pause, all four steps, run-to-cursor and both
breakpoint buttons are grayed out. Other useful buttons are enabled.

The auto-gray-out is not hard-coded. `Re-probe` asks the chip (`tlsr_dbg_present()`), and
a register read that ever returns real content switches the buttons on by
itself, so a different part or a future entry sequence needs no code change.

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
