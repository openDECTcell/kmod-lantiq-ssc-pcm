# 7412 bench tools (live DECT peer)

Throw-away research tools from the 2026-10-28 bench, kept because they found
the answer. They are DECT-specific (they speak the openDECTcell mail framing,
`opendectcell/docs/stub-api.md` §1), so they are **not** part of the generic
transport; they only use its uapi.

- `probe2.c` — cross-build like `ssc-pcm-test` (see `tools/build.sh`). Starts
  `/dev/ssc-pcm0` with N slots, waits for the peer's clock, measures the bit
  offset of 16-bit lanes from the `0xFEFE` idle lane, then sends the contact
  mail `0x22→0x02 0xF600` in every slot × in-slot byte order × bit shift and
  writes everything received to a raw file plus a probe log.
  `probe2 SLOTS RAW LOG SETTLE_MS -1` measures and picks shifts;
  `... 0 passive` only captures 12 s. Stream RAW off the box
  (`RAW=/dev/fd/3 3>&1 1>&2 | gzip`): a 30 s capture is 33 MB and the box's
  `/tmp` is RAM.
- `scanmail.py RAW LOG FRAME_BYTES` — finds `0xAA`-framed, LE16-sum-valid
  mails at every bit offset, slot and byte order (numpy).
- `wordloss.py RAW...` — word-loss rate from the spacing of a once-per-frame
  word (18-word frames).

Order that worked: `insmod lantiq_ssc_pcm.ko` (holds GPIO7), `insmod opendectcell`,
start `probe2`, then `dect-fwload ... --mode 0x119` with the 7412's 06.88
blobs. Result: periodic `0xF618`/`0xF623`/`0xFFF1` and a `0xF601` contact
answer at TX shift = measured lane offset, low byte of the slot first.
