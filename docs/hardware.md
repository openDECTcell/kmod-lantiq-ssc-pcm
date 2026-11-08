# FRITZ!Box 7412 — SSC-as-PCM/TDM hardware facts

The DECT audio path on the FRITZ!Box 7412 (board `avm,fritz7412`) is carried over the
Lantiq XWAY VR9 **Synchronous Serial Controller (SSC)** run as a PCM/TDM **slave**, not
over the SoC's native TDM block. The clock master is the DECT baseband peer (Dialog
SC14441), which drives the bit clock and frame sync; the SSC shifts fixed-size words in and
out under that external clock. This page is the hardware reference for the transport; the
live bring-up log is in `docs/7412-hardware-bringup.md`.

## 1. Block and platform
- SoC: Lantiq XWAY VR9 (`lantiq/xrx200`), MIPS, **big-endian**.
- SSC register window: physical `0x1E100800`, size `0x100` (KSEG1 `0xBE100800`).
- Registers are **native-endian**: use `__raw_readl`/`__raw_writel`. The byte-swapping
  `readl`/`writel` corrupt every access on this big-endian SoC.
- Module clock: the PMU SPI gate (bit 8), clkdev keyed to device name `1e100800.spi`; the
  gate is off until a driver enables it.
- Never read past the `0x100` window (a read into the unmapped space just above the block
  bus-errors) and never poke EBU/PMU registers at guessed addresses (both caused reboots
  during bring-up).

## 2. Register map
| Offset | Name | Meaning |
|---|---|---|
| 0x00 | CLC | clock control (RMC field at bits 8..15) |
| 0x04 | PISEL | port input select |
| 0x08 | ID | module / FIFO identification |
| 0x10 | CON | control |
| 0x14 | STATE | status |
| 0x18 | WHBSTATE | write-hardware-modified control (enable/disable/error strobes) |
| 0x20 | TB | transmit buffer |
| 0x24 | RB | receive buffer |
| 0x30 | RXFCON | RX FIFO control |
| 0x34 | TXFCON | TX FIFO control |
| 0x38 | FSTAT | FIFO status |
| 0x40 | BR | baud-rate reload |
| 0x44 | BRSTAT | baud-rate status |
| 0x60 | SFCON | serial-framing control (unused here) |
| 0x64 | SFSTAT | serial-framing status |
| 0x70 | GPOCON | GPO control |
| 0x74 | GPOSTAT | GPO status |
| 0x78 | WHBGPOSTAT | GPO force |
| 0x80 | RXREQ | RX request count |
| 0x84 | RXCNT | RX count |
| 0xEC | DMACON | DMA control |
| 0xF4 | IRN_EN | interrupt node enable |
| 0xF8 | IRN_CR | interrupt node capture |
| 0xFC | IRN_ICR | interrupt node control |

## 3. Register bits
- **CON (0x10):** data-width field (width−1) at `0x001F0000`; byte-valid enable `0x00400000`;
  clock polarity PO `0x40`; clock phase PH `0x20`; MSB-first `0x10`. For 32-bit, MSB-first,
  PO=1, byte-valid on: `CON = 0x005F0050`.
- **WHBSTATE (0x18):** DISABLE/config `0x1`, ENABLE/run `0x2`, SLAVE `0x4`; error-clear
  strobes combine to `0xF50`. **Write-only** — it reads back as 0, so never
  read-modify-write it; always write the command bits.
- **STATE (0x14):** bit0 = enabled. Enabled+slave reads `0x5`; slave+config reads `0x4`.
  RX overflow bit 9, TX underflow bit 11 — but note these did **not** set while words were
  demonstrably lost, so do not rely on them (detect loss from the data instead).
- **FSTAT (0x38):** RX fill level bits 5..0, TX fill level bits 13..8.
- **ID (0x08):** reads `0x08084528` here = 8-word TX and RX FIFOs, revision 8.
- Power-on values: `STATE=0x4`, `CLC=0x2`, `RXFCON=TXFCON=0x100`, `CON=0x00800000`.

## 4. Initialisation (slave; values exact)
1. `WHBSTATE = 0x1` (config/disable); poll `STATE` bit0 == 0.
2. `CLC = 0x100` (reduced module clock, RMC=1).
3. `BR = 0x1` (baud reload).
4. `RXFCON = 0x3 | (ITL << 8)` and `TXFCON = 0x103` (FIFO enable + flush; the flush bit
   self-clears). Bench with RX ITL=4, TX ITL=1 reads back `RXFCON=0x401`, `TXFCON=0x101`.
5. `WHBSTATE = 0xF50` (slave mode + clear all error strobes). `0xF54` also works.
6. `CON = 0x005F0050` (32-bit, MSB-first, PO=1, byte-valid on).
7. `IRN_EN = 0` (interrupts enabled later per engine; see §6).
8. `WHBSTATE = 0x2` (run/enable); poll `STATE` until enabled.

Enable/disable at runtime is the same WHBSTATE bit0/bit1 toggle followed by spinning on
`STATE` bit0 (≈100 ms timeout).

## 5. Pins (board GPIOs; gpio-xway base 512)
| GPIO | Role (slave mode) | Pad |
|---:|---|---|
| 7 | enable gate for the peer's bit clock | GPIO out, driven high before the peer leaves reset |
| 9 | frame sync from the peer | GPIO in (pull-up), not muxed |
| 10 | SSC chip-select 4 | out (xway group `spi_cs4`) |
| 16 | SSC data out (SoC → peer) | out (xway group `spi_di`) |
| 17 | SSC data in (peer → SoC) | in (xway group `spi_do`) |
| 18 | bit clock from the peer | in (xway group `spi_clk`) |
| 33 | peer reset (owned by the DECT driver, not this one) | untouched |

In slave mode the SSC transmits on the master-**input** pin and receives on the
master-**output** pin, so `spi_di` is the output and `spi_do` the input. pinctrl-xway pad
"output" config is param 4, packed `(4<<16)|on` (`0x00040001` out, `0x00040000` in).

## 6. Frame, FIFO and interrupt behaviour
- **No frame sync reaches the SSC.** When enabled while the peer is already clocking, the
  SSC's words begin at an arbitrary bit offset into the frame (observed 2, 3 or 5 bits on
  different boots, constant within a boot). The hardware never re-aligns, so the consumer
  must align in software from the data, **or** the SSC must be enabled before the peer's
  clock starts.
- **Word format:** each 32-bit word is two 16-bit slots, first slot in bits 31..16. An idle
  slot is `0xFEFE`.
- **FIFO/IRQ engine:** per frame, drain RX until `FSTAT` RX level reaches the frame's word
  count, flush both FIFOs, write one frame of words to `TB`; cap catch-up at two frames.
  RX request uses `IRN_EN` bit1, TX uses bit0, with the request level at half the FIFO.
  The 8-word FIFO fills every tick at the higher frame rates; a CPU path cannot keep up at
  18 words/frame.
- **DMA engine (needed at the stock rate):** VR9 central DMA port 2, RX channel 12, TX
  channel 13, 2-word bursts, `DMACON = 3`, FIFO request level 4; 192 one-frame descriptors
  per direction collected from a 1 ms timer; DMA words are in FIFO byte order.

## 7. Live bus geometry (set by the peer)
The peer's PCM configuration fixes the frame; the transport must be told the same geometry.
Frame period is 125 µs.

| Peer PCM mode | 32-bit words / frame | Bit clock | CPU engine |
|---|---:|---:|---|
| 4-word mode | 4 | 1.024 MHz | loss-free |
| 8-word mode | 8 | 2.048 MHz | near loss-free |
| 16-word mode | 16 | 4.096 MHz | not measured |
| 18-word mode (as shipped) | 18 | 4.608 MHz | lossy on CPU; DMA = loss-free |

A smaller configured frame on a wider bus makes the frame counter read proportionally high,
so configure the slot count to the peer's actual frame.

## 8. Peer / clock detection
The frame-sync and bit-clock frequencies can be measured on the input GPIOs to confirm the
peer is up and clocking before the link is declared active.
