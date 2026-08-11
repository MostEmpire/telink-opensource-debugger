# blue-pill-firmware

Firmware for the STM32F103 "Blue Pill" that turns it into a USB-CDC (UART) to SWire
bridge, that can program Telink TLSR825x chips.

## Build the firmware

```
make            ->  ../bin/blue-pill-firmware.bin, ../bin/blue-pill-firmware.hex
make test       ->  native unit tests for the SWire codec (needs a host gcc)
make clean      remove ./build, keep the released images
```

Needs `arm-none-eabi-gcc` and `make`. The images are written straight into
`../bin/` for simplicity. Objects stay in `./build`.

## Flash the firmware

Connect an ST-Link to PC's USB port and the SWD pins to the SWD header of Blue Pill and program
`../bin/blue-pill-firmware.hex` with STM32CubeProgrammer, ST-LINK Utility, or:

```
make flash-openocd                    # ST-Link via OpenOCD
make flash-serial SERIAL=COM3         # UART bootloader, BOOT0 high
```

After programming, plug in the Blue Pill to PC using the USB connector.
It enumerates as a virtual COM port (VID 0483 / PID 5740), which
Windows 10 and 11 should automatically recognize (if not, install ../driver).

## Hardware changes that turn Blue Pill into a programmer
Blue Pill needs 3 wires to write/read the memory of TLSR825x chips.
The SWS write/read (2 in one lines), GND and VCC. The board is designed to supply VCC in two ways. Low-power, using PB0 pin, which will be toogled high and will supply the chip with power. The supply over the GPIO driver is however limited and that's why I introduced a MOSFET variant. PB12 will drive the P-MOS, which can supply plenty of power to the Telink chip, so it won't brown-out even during higher consumption.

```
PA7   SPI1 MOSI  --[750R]--  target SWS      drives the line
PA6   SPI1 MISO  ----------  target SWS      senses the line
PA5   SPI1 SCK               not connected   (routed out only for scoping)
PB0              ----------  target +3.3V    direct GPIO supply
PB12             -- P-MOSFET gate            high-side switch, ACTIVE LOW
PC13             on-board LED                lit once USB is configured
PA11/PA12        USB D-/D+                   the Blue Pill's own socket
```

## Explanation of the folder content

| File | Purpose |
|---|---|
| `main.c` | Command dispatcher over USB CDC. |
| `usb.c` / `usb.h` | Bare-metal USB full-speed CDC-ACM device. No HAL. |
| `swire.c` / `swire.h` | SWire master: SPI1 + DMA, MSPI flash, target power. |
| `swire_codec.c` / `.h` | The cell encoder and decoder.|
| `startup.c` | Vector table, reset handler, 72 MHz / 48 MHz-USB clocks. |
| `stm32f103.h` | Register definitions; no CMSIS dependency. |
| `stm32f103.ld` | Linker script (64 K flash, 20 K RAM). |
| `test/test_codec.c` | Host unit tests for the codec. |

## Design notes

**Why SPI instead of bit-banging.** A SWire cell is a few hundred nanoseconds,
so any interrupt landing mid-frame corrupts it. Driving MOSI from DMA makes the
waveform immune to CPU jitter, and because SPI is full duplex the same transfer
samples MISO several times per cell — the master reads the line, including its
own echo, for free.

**Why the codec is a separate file.** `swire_codec.c` touches no hardware, so it
is unit tested natively (`make test`, 102 checks). On real hardware the receive
buffer is the transmit buffer plus whatever the target pulled low, which makes
the loopback exactly reproducible on a PC.

**Why the decoder measures instead of sampling at a fixed offset.** Each cell is
located by its own falling edge and classified by how long the line stays low,
so sampling skew does not accumulate. Slave bytes get their own threshold
derived from the target's observed cadence, because it replies on its own
oscillator rather than ours.

**Why `-O3`.** The encode and decode loops dominate transfer time. Building for
size instead cost roughly a third of the throughput, and there is 50 KB of
flash to spare.

## Blue Pill UART communication frames

```
request : 0x55 | CMD | LEN_LO | LEN_HI | payload
reply   : 0x55 | STS | LEN_LO | LEN_HI | payload
```

| CMD | Name | Payload → reply |
|----:|------|---|
| `0x01` | PING | — → `"TLSRSWS2"` + version |
| `0x02` | SYNC | `[div]` |
| `0x03` | SWS_WRITE | `[a2 a1 a0][data…]` |
| `0x04` | SWS_READ | `[a2 a1 a0][n16]` → n bytes |
| `0x05` | RESET | `[mode]` 0 soft, 1 power pulse, 2 power-cycle+activate |
| `0x06` | SET_SPEED | `[div 0..7]` |
| `0x07` | PWR | `[0/1]([rails])` → rail mask |
| `0x08` | SET_CFG | nine timing bytes |
| `0x09` | GET_CFG | — → cfg, version, power, GPIO state |
| `0x0A` | ACTIVATE | `[n16]` → frames sent |
| `0x0B` | GET_RAW | `[off16][n16]` → raw MISO samples |
| `0x0C` | SELFTEST | — → `[ok][echoed header]` |
| `0x0D` | PINTEST | — → `[ok][PA6 low][high][released]` |
| `0x0E` | ACT_READ | `[n16][a2 a1 a0][len]` → interleaved activate+read |
| `0x0F` | FLASH_RD | `[a2 a1 a0][n16]` → n flash bytes |
| `0x10` | FLASH_WR | `[a2 a1 a0][data…]` page program + WIP poll |

Status: `0x00` OK, `0x01` bad command, `0x02` bad length, `0x03` SWire timeout,
`0x04` no sync, `0x05` busy, `0x06` target power off.

`GET_RAW` returns the raw MISO samples of the last frame — the bridge is its own
logic analyser, which is how the protocol was worked out in the first place.
