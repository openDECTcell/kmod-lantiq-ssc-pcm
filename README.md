# kmod-lantiq-ssc-pcm

> **Note:** this module was written by an AI coding agent under the direction of the openDECTcell maintainers; only smaller
> parts have been reviewed by a human. It is provided without any warranty, is purely experimental and has been tested only on
> the hardware named below.
> See `docs/7412-hardware-bringup.md`.

A generic Linux kernel driver for the **Lantiq VR9 Synchronous Serial
Controller (SSC) used as a PCM/TDM slave** ("SPI-TDM"). A peer device supplies
the bit clock and frame sync; the controller shifts fixed-size PCM frames and
the driver exposes them as a stream through `/dev/ssc-pcm0`.

It is deliberately **generic**: it knows nothing about the device on the other
end of the link. On the AVM FRITZ!Box 7412 the peer is a Dialog SC14441 DECT
baseband, but that is a *user* of this transport, not part of it.

## Status

Used by openDECTcell on the FRITZ!Box 7412: the driver builds as an OpenWrt package (`kmod-lantiq-ssc-pcm` →
`lantiq_ssc_pcm.ko`), exposes `/dev/ssc-pcm0`, configures the SSC as a slave and carries DECT audio and control frames in
both directions (calls were made over it). Experimental; see `docs/7412-hardware-bringup.md`.

Target: **OpenWrt 25.12.x, Linux 6.12**, target `lantiq/xrx200`.

## Design

- Device tree driven: a `lantiq,xrx200-ssc-pcm` node (see
  `docs/binding-lantiq-ssc-pcm.yaml`) provides registers, interrupts, clock and
  pinctrl. On a board where the SSC is a TDM slave, that node replaces the
  disabled `spi@e100800` master node (`docs/dts-avm-fritz7412.dtsi`).
- Uses the kernel subsystems: clk framework for the module clock (PMU gate),
  pinctrl for pin muxing, `devm_*` resources.
- Because the stock 7412 image has no such node and the kernel has no device
  tree overlays, the driver has a small, clearly separated **bring-up path**
  (`ssc_pcm_fallback_*` in `src/lantiq_ssc_pcm_core.c`) that instantiates itself from
  the machine compatible so the module is installable and testable today. The
  device tree path is the one to keep.

## Layout

| Path | Contents |
|------|----------|
| `Makefile` | OpenWrt package `kmod-lantiq-ssc-pcm` |
| `src/lantiq_ssc_pcm_core.c` | platform driver, DT binding, `/dev/ssc-pcm0` |
| `src/ssc_pcm_hw.c/.h` | SSC slave transport engine (steps, FIFO, ring) |
| `src/include/uapi/ssc_pcm.h` | transport uapi |
| `docs/` | hardware facts, binding, findings |
| `boards/avm,fritz7412` | board facts used by the bring-up path |
| `tools/build.sh` | build with the OpenWrt SDK into `deploy/` |
| `tools/ssc-pcm-test.c` | bring-up step exercise tool |

## Build and run

```
tools/build.sh [path-to-openwrt-sdk]     # stages deploy/lantiq_ssc_pcm.ko and the .apk
```

On the device (OpenWrt 25.12 uses apk):

```
apk add --allow-untrusted /tmp/kmod-lantiq-ssc-pcm-*.apk   # or insmod lantiq_ssc_pcm.ko
ls -l /dev/ssc-pcm0
```

`tools/ssc-pcm-test` runs the bring-up step sequence
(1 CLOCK, 2 IFACE, 3 TDM, 4 DMA) and dumps the controller registers.

## Relationship to openDECTcell

This is the transport; [openDECTcell](https://github.com/openDECTcell/opendectcell)'s DECT stack (firmware loader, call control, audio) is a
consumer. openDECTcell adapts to this transport's conventions rather than this module mimicking openDECTcell's interface.

## License

GPL-2.0-only (the uapi header: GPL-2.0 WITH Linux-syscall-note; the binding: GPL-2.0-only OR BSD-2-Clause). The texts are in
`LICENSES/`. This module was written by an AI agent with the vendor's published GPL driver sources as a reference for how
the SSC behaves; its hardware facts come from those sources and from observing the hardware. It was not written inside the
project's clean-room process. A mechanical line comparison of its code against the whole kernel tree of the vendor's 7362 SL
GPL drop (2026-11-08; that tree holds the same Infineon SSC driver) found no copied code: at most a few percent of lines match
anything, and those are generic kernel idioms. The 7412 drop itself was not compared.

## Provenance

The hardware facts in `docs/` come from public sources (data sheets, the kernel's own drivers, the vendor's published GPL
source drops) and from observing the hardware; where a fact comes from the clean-room record of the openDECTcell project it is
labelled `[CR ...]` or `A-nnnn`, see [opendectcell's clean-room-process.md](https://github.com/openDECTcell/opendectcell/blob/main/docs/clean-room-process.md).
No vendor firmware, binaries or kernel modules are in this repository.
