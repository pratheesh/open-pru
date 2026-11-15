**Branch note:** the silicon results below were measured on firmware at 8aac84c1 plus the AM263x port. This branch (rnd91-am263-i2c-pr172) re-targets PR #172 head 8f4c1c2c (one IRQ flag byte per instance), which is not yet run on silicon.

# RND-91 results: PRU I2C master on LP-AM263, silicon (2026-11-15)

**Verdict: PASS.** All ten arms passed on silicon (read-only and write, at 100 kHz and 367.6 kHz). The PRU1 I2C
master on J8-73/J8-74, jumpered to I2C1 J5-50/J5-49, talks to the on-board CAT23M01 EEPROM and TPIC2810. Every
result is checked against an independent R5F MCU I2C1 read.

Board am263 (LP-AM263, probe S26B0004), ICSSM PRU1 at 200 MHz. The board was taken through `waitrun` after a clean
ETEST-27 handoff, then given an outlet-4 power cycle (JTAG ok, 4 IR bits). Raw result blocks, `done.json` and
`ccs.log` for both runs are in `bench/results/<tag>/`. Decode them with `decodeBlock` in `bench/rnd91_lib.mjs`.

## Run 2: `rnd91-b2-20261115-1611`. PASS, 35 s, image r5f 4665959d, pru1 31bd4735 (commit fa06c035)

| arm | result | evidence |
|---|---|---|
| R0 | PASS | MCU read of 0x53:0x0400 = `00 01 .. 0f` |
| P1@100k | PASS | ACK (0x500) at 0x61, 0x52, 0x53; NACK (0x508) at 0x10, 0x21 |
| P2@100k | PASS | PRU 16-byte read of 0x53:0x0400 = R0 |
| C3 | PASS | MCU only: `[0x11,0x5A]`, then a read returns 0x5A, then `[0x22]`. After `[0x44,0xA5]` the read returns 0xFF (recorded, not gated) |
| P3@100k | PASS | PRU `[0x11,p]` ACK, MCU read = p, PRU `[0x22]` ACK, for p = 0x55, 0xAA, 0x00 |
| P4@100k | PASS | orig `00..07`, PRU wrote `ff fe .. f8`, MCU readback equal, PRU restore, MCU readback `00..07` |
| P1@400k | PASS | same ACK/NACK map at 367 647 Hz (IEP increment 0x2A8, tLOW floor) |
| P2@400k | PASS | = R0 |
| P3@400k | PASS | same as at 100k |
| P4@400k | PASS | same as at 100k; EEPROM restored |

Every PRU stop read back CONTROL = 0x1 (core disabled) and OUT_CTRL = 0x20080 (bits 17 and 7 set, so both lines are
released) before the MCU driver opened.

## What this establishes on silicon

These were UNVERIFIED items in `examples/pru_i2c/am263x-lp-bench.md`:
- SDA = `PR0_PRU1_GPIO17` (bit 17) and SCL = `PR0_PRU1_GPIO7` (bit 7) on balls D13/A16.
- PRU1 can store to MSS_CTRL `ICSSM_PRU1_GPIO_OUT_CTRL` at 0x50D00828 while the R5F holds MSS_CTRL unlocked.
- OUTDISABLE = 1 releases the pad; clearing it drives the pad low (an open-drain emulation).
- Halt, load and run of the R5F image per arm, with no CPU reset, comes up clean ten times in a row.
- TPIC2810 readback works after 0x11 (shift register) and returns 0xFF after 0x44. Run 1's P3 failure was a defect in
  the bench design, not in the PRU master.
- The CAT23M01 write-pointer-then-read sequence and the 8-byte page write work through the PRU master.

## Not established

- **Waveform timing.** The SCL rates are the firmware's IEP arithmetic, and no scope or analyser was used. "400 kHz" is
  367.6 kHz by design (tLOW floor). Rise time with 4.7 k pull-ups and the jumper wires is unmeasured.
- **Sample size.** One session per configuration, with three LED patterns and 8 EEPROM bytes per rate. These are
  functional results, not a long-run error rate.
- **Visual LED state.** It was not observed. The final pattern 0x00 leaves the LEDs off.

## Run 1: `rnd91-b1-20261115-1547`. FAIL at P3@100k (kept for the record)

R0, P1@100k and P2@100k passed. In P3 the PRU sent `0x44 <p>` (ACKed) and then the MCU read 0x61, which returned 0xFF
for p = 0x55, 0xAA and 0x00. The SDK example `i2c_led_read_write_lld.c` reads only after 0x11 and loads with 0x22.
C3 in run 2 reproduced the 0xFF after 0x44 with the MCU alone, which confirms the diagnosis.
