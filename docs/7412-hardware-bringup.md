# FRITZ!Box 7412 hardware bring-up notes

Findings from a live OpenWrt 25.12.6 image on an AVM FRITZ!Box 7412
(`lantiq/xrx200`, Linux 6.12.112, board compatible `avm,fritz7412`).

> **Superseded diagnosis (kept so nobody repeats it).** Sessions on 2026-10-26
> concluded that writes to `0x1e100xxx` were blocked by an EBU write-protect.
> That was wrong: the driver used `readl()`/`writel()`, which byte-swap on this
> big-endian SoC, so every write hit the wrong bits and every readback was
> swapped. **Do not poke EBU
> registers** — no EBU access is needed, and a guessed EBU write rebooted the
> box.

## Environment

- OpenWrt 25.12.6, r33311, kernel 6.12.112, mips_24kc, packages are **apk**.
- Device tree keeps `spi@e100800` (`lantiq,xrx200-spi`) `status = "disabled"`,
  and no node describes the DECT/SSC link. `spi-lantiq-ssc` is built in
  (`CONFIG_SPI_LANTIQ_SSC=y`) but unbound.
- `CONFIG_OF_OVERLAY` is **not** set, so device tree nodes cannot be added at
  runtime. A DT-driven driver therefore needs a rebuilt DTB/image; that is the
  reason for the module's bring-up path.
- GPIO chip `gpio-xway`, base 512 (GPIO n = gpio-(512+n)). GPIOs 7, 9, 10,
  16–18 and 33 are unclaimed on the stock image.

## Verified on hardware (2026-10-28)

With native accessors (`__raw_readl`/`__raw_writel`, commit `c75698e`):

| Register | Value | Meaning |
|---|---:|---|
| `ID` | `0x08084528` | TXFS = RXFS = 8 words, REV 8 |
| `CLC` after step 1 | `0x00000100` | RMC = 1 written and read back |
| `CON` after step 3 | `0x005f0050` | 32-bit, MSB first, PO, byte-valid |
| `RXFCON` / `TXFCON` | `0x00000401` / `0x00000101` | ITL 4 / 1, enabled (flush bit self-clears) |
| `STATE` after enable | `0x00000005` | enabled + slave |
| `STATE` after disable | `0x00000004` | slave, configuration mode |

- Power-on values (native): `STATE = 0x4`, `CLC = 0x2`, `RXFCON = TXFCON =
  0x100`, `CON = 0x00800000`.
- The module clock comes from the kernel's clkdev entry `1e100800.spi` (PMU
  SPI gate); the fallback takes it with `clk_get_sys()` instead of writing the
  PMU itself.
- GPIO7 (`enable`) is driven high at probe and shows `out hi` in
  `/sys/kernel/debug/gpio`.
- Pin mux applied by the fallback (`/sys/kernel/debug/pinctrl/.../pinmux-pins`):
  io10 `spi_cs4`, io16 `spi_di`, io17 `spi_do`, io18 `spi_clk`, all function
  `spi`; pad configs io16 output, io17/18 input (io10 was input on
  2026-10-28, switched to output after [CR A-0001] — see "Pin roles").
- With the SSC enabled and no peer clock, `FSTAT` stays 0 for 1 s. This says
  only that no bit clock reached the SSC (the peer was not started); it is not
  a test of the receive path.

## The live bus (2026-10-28, peer = SC14441 running the 7412's 06.88 STUB)

Observed with this driver's interrupt engine and raw captures. These are
properties of the peer's configuration, not of the SSC, but they decide what
the transport has to carry:

| Peer mode byte | 32-bit words per 125 µs frame | Bit clock | CPU engine |
|---|---:|---:|---|
| 0x101, 0x105 | 4 | 1.024 MHz | loss-free (44,799 frames) |
| 0x109 | 8 | 2.048 MHz | near loss-free |
| 0x111, 0x115 | 16 | 4.096 MHz | not measured |
| 0x119 (stock), 0x11D | 18 | 4.608 MHz | loses 1–3 % of frames; **DMA engine: no loss** (125,599 frames) |

- **DMA engine (default since `da43ff4`):** VR9 central DMA port 2, RX
  channel 12, TX channel 13, 2-word bursts, `DMACON = 3`, FIFO request
  levels 4 [CR A-0006]; 192 one-frame descriptors per direction, collected
  from a 1 ms timer. DMA words are in FIFO byte order (`dma_swap=0`).
- **Control channel verified through this transport:** with the transport
  started before the peer boots (mode 0x119), a contact mail queued with
  `write()` drew the STUB's first-contact mails (`0xFFF2`, `0xF601`,
  `0xF602`) and then periodic reports every 5 s, all in one fixed receive
  slot. In that session: lane offset 6 bits, TX slot 29, RX slot 9, low byte
  of each slot first (both found by `tools/bench-7412/probe2`).

- **Engine:** SSC RX request (IRN_EN bit 1 → ICU line 22) with the request
  level at half the FIFO, draining until empty. IRN_EN bit 0 raises line 23
  (TX). The 8 kHz hrtimer found the 8-word FIFO full on every tick and lost
  words without any error flag; at 18 words/frame no CPU path keeps up
  (request level 4 or 2, either CPU) — that rate needs DMA, as the stock
  driver uses [CR A-0002].
- **No error flags:** STATE bits 9 (RX overflow) and 11 (TX underflow) stayed
  0 while words were demonstrably lost. Detect loss from the data (a periodic
  word drifting by a word) instead.
- **Bit alignment:** there is no frame sync into the SSC. Enabled while the
  peer clocks, the SSC's words start at an arbitrary bit offset into the
  frame — 2, 3 and 5 bits on different boots, constant within one. Words are
  never re-aligned by the hardware, so a consumer must align in software
  (or the SSC must run before the peer's clock starts, as stock does).
- **Bus rates in the stats:** a 4-word default frame on an 18-word bus makes
  `frames` read 4.5 × too high; configure `slots` to the peer's frame.

## Pin roles

pinctrl-xway names the SPI pins for master mode. In slave mode the SSC
transmits on the master-input pin and receives on the master-output pin:

| GPIO | xway group | Role in slave mode | Pad |
|---:|---|---|---|
| 7 | — | enable for the peer's bit clock | GPIO out, high before peer reset |
| 9 | — | frame sync from the peer (stock: input with pull-up) | GPIO in, not muxed |
| 10 | `spi_cs4` | SSC chip select 4 | out, as stock |
| 16 | `spi_di` | SSC data out | out |
| 17 | `spi_do` | SSC data in | in |
| 18 | `spi_clk` | bit clock from the peer | in |
| 33 | — | peer reset (consumer's business) | untouched by this driver |

## Safety notes (learned the hard way)

Two kernel panics/reboots were caused during the 2026-10-26 bring-up:

1. Reading **past the SSC block** (offsets 0x900/0x1000 in a 0x1000 mapping)
   into unmapped FPI space → bus error → panic. Only ever touch the documented
   0x100-byte block (0x1e100800..0x1e1008ff).
2. Writing a guessed EBU address (`0x1e105300 + 0x64`) → panic. Never poke
   EBU/clock registers at an unverified address.

Set `sysctl -w kernel.panic=10` before experiments and use `timeout` on SSH
commands so a crashed box reboots and does not hang the shell.

## Stable facts

- SSC physical base `0x1e100800`, size `0x100`; register map matches mainline
  `spi-lantiq-ssc`. Registers are native-endian: use raw accessors.
- Module clock = PMU SPI gate `BIT(8)`; clkdev entry keyed to dev name
  `1e100800.spi` (mainline registers it with `deactivate = 1`, i.e. the gate is
  **off** until a driver enables it). On xway the legacy `clk_enable()` returns
  -1 for a NULL clock, so `devm_clk_get_optional_enabled()` cannot be used.
- pinctrl-xway pad config "output" is param 4, packed `(4 << 16) | on`
  (`0x00040001` out, `0x00040000` in), as listed in `pinctrl-maps`.
- SSC register bits: `WHBSTATE` DISABLE=0x1, ENABLE=0x2, SLAVE=0x4,
  error clears `0xf50` (the stock image writes `0xe54`; `0xf54` also works).
  `WHBSTATE` reads back as 0 and must never be read-modify-written.
  `CON` for 32-bit MSB-first PO=1 byte-valid = `0x005f0050`; `CLC` RMC field at
  bits 8..15.
