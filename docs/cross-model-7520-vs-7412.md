# 7520/7530 vs. 7412: what differs, and what openDECTcell must adapt

Requested by the maintainer: `opendectcell` currently targets the FRITZ!Box 7520
(IPQ4019 ADSS TDM). The 7412 uses a **different PCM block** (VR9 SSC in SPI-slave
mode). This document lists every difference that affects `opendectcell` and what has
to change there. Facts carry their source tags: **[7412-DOC]** = openDECTcell's
`docs/7412-spi-tdm-and-loader.md` / `docs/models/7412-DECT-analysis.md`,
**[HW]** = this project's `docs/hardware.md`, **[7520-DOC]** =
openDECTcell's `docs/pcm-tdm-ipq4019.md` / `docs/kmod-spec.md`.

## 1. Summary

Both boxes put the **Dialog SC144xx DECT baseband in charge of the PCM bus**: the
chip is the clock master, the SoC is the slave, and the control "mails" ride in
one PCM slot on an 8 kHz frame. The host stack above that (chip firmware loader,
mail protocol, DECT NWK) is essentially the same [7412-DOC §3]. **Only the SoC
side of the PCM transport is different**, and only that has to be re-implemented.

| Aspect | 7520 / 7530 | 7412 |
|---|---|---|
| SoC / arch | Qualcomm IPQ4019, ARMv7 (LE) | Lantiq VR9 (xrx200), MIPS 34K (BE) |
| PCM hardware block | IPQ4019 **ADSS TDM** | **VR9 SSC** in SPI-slave mode |
| Clock master | DECT chip (observed) | DECT chip (assumed; SSC programmed as slave) |
| Data unit on the bus | 16-bit slots, I2S/TDM | 32-bit words shifted by the SSC, byte-valid |
| Frame geometry | 8 slots × 16 bit = 128 clk = 16 B, FSC 8 kHz, DCL 1.024 MHz (observed) | 128 clk / 16 B likely; SSC packs into 4 × 32-bit words/frame (**open**) |
| Reset GPIO | 40 (active low) | 33 (active low) |
| PCM pins | GPIO 20-23 (rx), 24-27 (tx) | FSC 9, DCL 18, DO 16, DI 17, clk-enable 7 |
| Boot strap | UART RX pin (GPIO 16) held low | **none** – ROM handshake on the console UART |
| Firmware-load UART | `msm_serial` `ttyMSM0` | ASC console `ttyLTQ0` |
| STUB family / mode | `488` / `0x105` | `441` / `0x119` |
| Kernel | 6.12.94 | 6.12 (same release) |
| Target | `ipq40xx/generic` | `lantiq/xrx200` |
| openDECTcell transport name | `ipq40xx-tdm` | `lantiq-ssc` |
| Kernel package | `kmod-opendectcell-ipq40xx` | `kmod-opendectcell-lantiq` |

## 2. Differences that require a code change in openDECTcell

### 2.1 A new transport driver (unavoidable)

`kernel/opendectcell_pcm.c` programs the IPQ4019 ADSS/MBOX/stereo blocks. None of that
exists on VR9. The 7412 needs a new transport driver for the VR9 SSC, in the same
shape (step-wise bring-up, `read()` block stream, stats), living in the same
`kmod-opendectcell-lantiq` module. This is what `kmod-lantiq-ssc-pcm` implements.

The natural upstream layout (openDECTcell `docs/hardware-abstraction.md` §3):

```
kernel/opendectcell_lantiq.c     # miscdevice, board table entry avm,fritz7412, Phase A
kernel/opendectcell_ssc.c/.h     # VR9 SSC slave transport (new)
kernel/include/uapi/opendectcell.h
```

### 2.2 `DECT_IOC_INFO.transport` and board table

No change to the interface; a new board-table entry keyed on
`compatible = "avm,fritz7412"` supplies reset/strap/pin data and the transport
name `lantiq-ssc`. The board is matched by DT compatible, as on ipq40xx.

### 2.3 `DECT_IOC_STRAP` is not applicable on the 7412

The 7412 has no UART-RX strap pin [7412-DOC §2]; the chip enters its ROM boot
loader directly on the console UART. The lantiq board entry should set no strap,
and `DECT_IOC_STRAP(1)` must return `-EOPNOTSUPP` (or be a no-op) rather than
touching a pin. `dect-fwload` must treat a missing `strap` capability as "not
needed" for the 7412.

### 2.4 PCM bring-up steps are transport-specific

The step names in `opendectcell.h` are ADSS-flavoured
(`CLOCK/IFACE/PADCLK/TDM/STEREO/DMA`). On the SSC the sequence is different:

| Step | ipq40xx (ADSS) | lantiq (SSC) |
|---|---|---|
| 1 CLOCK | GCC audio clocks, ADSS readable | enable SSC module clock (CLC / PMU), regs readable |
| 2 IFACE | audio bus branches on | put GPIO 9/16/17/18 into SPI-TDM/alt function, clk-enable 7 high |
| 3 PADCLK | I2S reset, pad clocks | *(no equivalent)* – SSC takes the external DCL directly |
| 4 TDM | TDM slave config, stereo, pins | SSC slave config: `WHBSTATE`, `CON`, `RXFCON`, `TXFCON` (§ below) |
| 5 STEREO | receiver on, counters run | *(no equivalent)* – use the SSC FIFO/status instead |
| 6 DMA | MBOX DMA ring | SSC FIFO + MBOX/DMA **or** FIFO + IRQ (**open**) |

Recommendation: keep the numeric step API (`DECT_IOC_PCM_STEP(n)`) but document
that the *meaning* of steps 3 and 5 differs per transport, or add a small
per-transport step-name table in `DECT_IOC_INFO`. The safest minimal change is to
keep the numbers and let the lantiq driver treat 3 and 5 as no-ops.

### 2.5 `struct dect_pcm_regs` is ADSS-specific

`dect_pcm_regs` carries ADSS/ADCC/stereo/MBOX register groups. For the SSC these
groups do not exist. Options for openDECTcell:

- add a transport id to `dect_ioc_info` and a small second layout for the SSC
  (e.g. reuse `glb[]` for the SSC registers CLC/CON/WHBSTATE/RXFCON/TXFCON), or
- add a new `DECT_IOC_PCM_REGS_SSC`.

Either way it is a uapi extension; the version counter should move.

### 2.6 No stereo counters, different stats

`dect_pcm_stats` fields `rx_samples_lsb/msb` come from the ADSS stereo counters.
The SSC has `FSTAT`/`SFSTAT` instead. Keep the struct but document that the two
counter fields are transport-defined.

### 2.7 Block/read/write model can be reused

`read()` returning `dect_pcm_block`-headed blocks and the `write()` TX-frame
queue are transport-independent; the lantiq driver should produce the same block
stream so `dect-pcmcap`, `dect-stubd` and the mail parser work unchanged.

### 2.8 Board profile

openDECTcell `boards/avm,fritz7412` needs: `transport=lantiq-ssc`, `fw_family=441`,
`dect_mode=0x119`, `uart=/dev/ttyLTQ0`, `uart_is_console=1`, and the
`pcm_*` keys once the frame layout is confirmed (see §3).

## 3. Open items carried over from openDECTcell's 7412 notes

These are the same unknowns listed in `docs/7412-spi-tdm-and-loader.md` §4 and
`docs/models/7412-DECT-analysis.md` §6, now with what this project found:

1. **DCL/FSC source and direction** – SSC is programmed as a slave (`WHBSTATE`
   clears master mode) [HW]; who *generates* DCL/FSC is unresolved. openDECTcell
   suspects a GPTU-timer FSC plus an external/GPIO7-gated DCL [7412-DOC §1].
2. **Words per frame** – the 7520 observed 8×16-bit = 16 B/frame; the SSC shifts
   32-bit words, so 4 words/frame is the likely packing (**assumed**).
3. **Control-slot position / voice-slot map** – unknown; the SSC gives 4 words
   per frame with no per-slot field, so the slot map must be taken from the STUB
   side (mode 0x119).
4. **DMA vs. FIFO+IRQ** for sustained streaming – AVM used DMA
   central-DMA channels plus a CPU "yield" handler [HW];
   a plain DMA ring driven by the SSC RX/TX FIFO request lines is the expected
   clean equivalent.
5. **Spare chip-select lines (GPIO 10/12)** – still unknown; not needed for PCM.

## 4. What this means for the implementation order

1. Get the SSC register block up and read back (`CLC`, `ID`, `CON`, `WHBSTATE`)
   – no clocks to the chip needed. (Done-ish in `kmod-lantiq-ssc-pcm`.)
2. Confirm pin function for GPIO 9/16/17/18 on lantiq/xrx200 (Lantiq "port"
   mux) and that no SPI-master driver claims the SSC.
3. Confirm the frame geometry with the chip running (the `DECT_IOC_CAPTURE`
   diagnostic already measures DCL/FSC).
4. Implement the receive path (FIFO or DMA) and validate control mails
   (gate G2), then transmit.
