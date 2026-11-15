# PRU I2C on the LP-AM263: PRU1 bench (RND-91)

Status: the first silicon run (rnd91-b1-20261115-1547) is recorded in "First silicon result" below; everything the
text does not mark as run on silicon is still desk work. The PRU firmware and the R5F app were built (clpru 2.3.3,
tiarmclang 4.0.4, SysConfig 1.28, MCU+ SDK AM263x 26.01.00.08) and the logic was tested on the host against a fake
bus. The AM263x TRM is not on the PC, so every register-level statement below that is not an SDK file citation is
marked UNVERIFIED.

## First silicon result (run rnd91-b1-20261115-1547, 100 kHz)

- **PASS on silicon:** R0 (R5F read of 0x53:0x0400), P1@100k (the PRU master ACKs 0x61/0x52/0x53 and NACKs 0x10/0x21: response words
  0x500 and 0x508), P2@100k (the PRU 16-byte read of 0x53:0x0400 equals the R5F read, `00 01 .. 0f`). So the pins, the OUT_CTRL
  release and the 100 kHz timing work for reads and address phases.
- **FAIL: P3@100k.** For patterns 0x55, 0xAA and 0x00 the PRU write `0x44 <pattern>` returned 0x500 (ACK) and the MCU read of 0x61
  returned status 0 with value 0xFF every time.
- **Diagnosis (desk, not proven):** the SDK example `examples/drivers/i2c/i2c_led_read_write_lld/i2c_led_read_write_lld.c` never
  reads after 0x44 (lines 43-46 and 105-135). Its verified sequence is write `[0x11, mask]` (write the shift register), read 1
  byte (returns the shift register, compared with the mask), then write `[0x22]` (load the outputs). A read after 0x44 is not a
  sequence the SDK relies on, so 0xFF there says nothing about the PRU. The 400 kHz arms and P4 had not run (stop at first failure).

## Wiring the bench has (fitted by hand)

| PRU signal | ball | header | goes to | I2C1 signal |
|---|---|---|---|---|
| `PR0_PRU1_GPIO17` (SDA) | D13 | J8-73 | J5-50 | `I2C1_SDA` |
| `PR0_PRU1_GPIO7` (SCL) | A16 | J8-74 | J5-49 | `I2C1_SCL` |

4.7 k pull-ups to 3V3 are on the board (R175/R176). Devices on I2C1: EEPROM CAT23M01 0x52 (board ID, never written) and
0x53 (written only at offset 0x0400 and up), TPIC2810 LED driver 0x61. I2C3 (boot-mode TCA6408 at 0x21) is never used.
The MCU I2C1 controller (pads D7/C8) is on the same two wires as the PRU, so the order of ownership matters, see
"Bus ownership".

## Pin and pad facts (cited)

- **Pad modes.** SysConfig device data `AM263x_beta.json` (one line, search the ids): `ID_2015` = ball A16 `SDFM0_CLK1`,
  pad control `0x531001F0`, mode 0 = `PR0_PRU1_GPIO7`; `ID_2024` = ball D13 `SDFM0_D1`, pad control `0x531001F4`,
  mode 0 = `PR0_PRU1_GPIO17`. SDK `source/drivers/pinmux/am263x/pinmux.h:273-274` has `PIN_SDFM0_CLK1 = 0x1F0` and
  `PIN_SDFM0_D1 = 0x1F4`; **there is no `PIN_PR0_PRU1_GPIO17` / `PIN_PR0_PRU1_GPIO7` define** (the header names the pads
  by their SDFM function). The app does not hand-write pad words: SysConfig (`example.syscfg`, `PR0_PRU1_GPIO7`,
  `PR0_PRU1_GPIO17`, `rx = true`, no pull) generates `ti_pinmux_config.c` in System_init. Pad bit fields: `pinmux.h:91-138`.
- **Bit numbering.** The SDK's own SysConfig template maps `PR0_PRU1_GPIOn` to bit n of `ICSSM_PRU1_GPIO_OUT_CTRL`
  (`source/sysconfig/drivers/.meta/pinmux/pinmux_config.c.xdt:162-165`, `1 << <GPIO number>`). So SDA = 17, SCL = 7. That R30/R31
  bit n is the same GPIO n is the naming rule the SDK uses; the TRM statement is **UNVERIFIED**. A P1 ACK from 0x61 proves both
  bit numbers on silicon.
- **Mux.** PRU1 GP mux mode 0 (GP) and GPI mode "direct" are set by the app (`PRUICSS_setGpMuxSelect`, `PRUICSS_setGpiMode`,
  SysConfig default is GP, `pruicss_m_v0_gpio_gp.syscfg.js:130-180`). `SOC_selectIcssGpiMux(1, 0)` selects GPI rather than PWMXBAR
  (`soc.h:509-514`, 0 = GPI).

## Open-drain: how the PRU releases the line (STOP-DECISION test: passed)

The AM263x has a per-pin output-driver disable for PRU GPOs: `CSL_MSS_CTRL_ICSSM_PRU0/1_GPIO_OUT_CTRL`, MSS_CTRL base `0x50D00000`,
offsets `0x824` / `0x828` (`source/drivers/hw_include/am263x/cslr_mss_ctrl.h:843-844`), field `OUTDISABLE`, 30 bits, reset 0
(`cslr_mss_ctrl.h:2889-2905`). The SDK's SysConfig template writes a 1 into the bit of every pin configured `rx` ("Set bits for input
pins", `pinmux_config.c.xdt:232-236`). So a pad can be tri-stated and push-pull is **not** the only option.

- The firmware never drives SDA or SCL high: R30 bits stay 0 and a line is "high" by setting its OUTDISABLE bit (released, the
  4.7 k pull-up lifts it) and "low" by clearing the bit (`pru_i2c_macro.h` `SET_SCL_PIN_HIGH` ...). Whatever the polarity of the
  bit turns out to be, no code path puts a 1 on R30, so a driven-high contention cannot happen.
- **Ported.** The firmware had the AM261x register addresses hard-coded (`0x50D00810/814`). `pru_i2c_main.asm` now selects
  `0x50D00824/828` under `-DSOC_AM263X`. `dispru` of the built image shows `LDI R26.w2, 0x50D0 / LDI R26.w0, 0x0828`.
- **MMR lock.** MSS_CTRL is a locked partition (`soc.c:163-176`); a PRU store while locked would be dropped, the lines would never
  go low and every probe would read NACK (a safe failure, no contention). The R5F therefore unlocks MSS_CTRL when it starts the PRU and
  locks it again only after the PRU is stopped and the lines are released. **UNVERIFIED:** that the PRU can reach `0x50D00828` at all
  (the AM261x design relies on the same path), and that OUTDISABLE = 1 releases (name + SDK template only).
- The `rx = true` pins start released: SysConfig writes the whole register at System_init with both bits set.

## Timing: 200 MHz

The firmware is IEP-tick driven. The IEP clock and the ICSSM core clock are both 200 MHz on AM263x
(`pruicss_m_v0.syscfg.js:78-98`, `iepClk` default and only option 200 MHz; lab.json `pruClockMHz` 200). The upstream am261x example
derives the compare increment from `CONFIG_PRU_ICSS0_IEP_CLK_FREQ_HZ` (250 MHz by default on AM261x, its own comment) while the
`IEP_CMP_INCREMENT_VAL_*` constants in `pru_i2c_interface.h` assume 200 MHz. On AM263x the two agree, and `rnd91_bench.h`
(`Rnd91_clksPerTick`, `Rnd91_iepIncrement`, `Rnd91_sclTimeoutTicks`) computes them from the generated clock define.

IEP counts 5 per clock (`DEFAULT_INC`). 4 ticks per SCL period. Tick = max(bus tick, tLOW floor):

| | 100 kHz | 400 kHz |
|---|---|---|
| bus tick, IEP clocks | ceil(200e6 / (4 * 100e3)) = **500** | ceil(200e6 / (4 * 400e3)) = 125 |
| tLOW floor (680 ns), clocks | ceil(200000 * 680 / 1e6) = 136 | 136 |
| clocks per tick | 500 | **136** (the floor wins) |
| compare increment = clocks * 5 | **2500 = 0x9C4** (= `IEP_CMP_INCREMENT_VAL_100KHZ`) | **680 = 0x2A8** (= `IEP_CMP_INCREMENT_VAL_400KHZ`) |
| SCL period / rate | 4 * 500 / 200e6 = 10 us / **100000 Hz** | 4 * 136 / 200e6 = 2.72 us / **367647 Hz** |
| SCL timeout (25 ms) in ticks | 25 * 200000 / 500 = **10000** | 25 * 200000 / 136 = **36764** |

"400 kHz" is therefore 367.6 kHz: at 400 kHz the firmware's own comment says tLOW would be 1.25 us, out of Fast-mode spec, so
the tick is held at 680 ns (tLOW >= 1.33 us). Both rates are checked in `bench/test_rnd91_host.c` against these literals.
The state-machine instruction cycles between edges are not re-counted; the 680 ns floor carries that margin (as in the upstream
comment). **UNVERIFIED** on a scope. Rise time with 4.7 k and the jumper wires at 367 kHz is not known.

## Bus ownership (the design rule, enforced and tested)

PRU and MCU I2C1 controller are never both live. `Rnd91_Guard` (in `rnd91_bench.h`) owns the bus: the PRU may start only if
nobody owns it; the MCU I2C1 driver may open only if nobody owns it; the PRU gives the bus back only when
`Rnd91_linesReleased()` holds: PRU CONTROL register shows the core disabled and not running (`PDSP_ENABLE` bit 1 and `PDSP_STATE`
bit 15 clear, `cslr_icss_m.h` `PDSP1_IRAM_CONTROL`) **and** both OUT_CTRL bits (7 and 17) are 1, read back after the R5F stopped the
core and set them. `main.c` does not call `Drivers_open()` (that would open I2C1 first); it calls `Drivers_uartOpen()` for the log and
`Drivers_i2cOpen()` / `Drivers_i2cClose()` (declared in the generated `ti_drivers_open_close.h`, the documented "no Drivers_open" path)
only through the guard. Events are logged in the block so the session can check the order after the fact.

## One arm per R5F run

The session writes a request into the result block (`REQ_MAGIC`, arm, Hz, cookie), loads the image, runs the core and reads the block when
`STATE` is DONE/ABORT. The app writes everything else. Layout (all u32, byte offsets from `0x70170000`, the lab's `msramScratch`, an MPU
region marks it non-cached so a JTAG read sees the R5F's writes): see the `RND91_OFF_*` defines in
`mcuplus/am263x-lp/r5fss0-0_nortos/rnd91_bench.h`; `bench/rnd91_lib.mjs` reads the offsets from that header (one copy).

| offset | content |
|---|---|
| 0x000-0x00C | magic `R91B`, version 1, state (0 cleared, 1 running, 2 done, 3 abort), stage counter |
| 0x010-0x01C | request: `R91Q`, arm (1 R0, 2 P1, 3 P2, 4 P3, 5 P4, 6 C3), Hz, cookie |
| 0x020-0x034 | error code, IEP increment, clocks/tick, SCL timeout ticks, frequency selector, SCL Hz |
| 0x040-0x0C0 | event log (32 words `code<<24 \| arg`) |
| 0x0C4-0x0E4 | per PRU stop (first four): CONTROL register and OUT_CTRL as read back; stops 5-8 at 0x240-0x25C |
| 0x100-0x114 | R0: MCU read status, 16 bytes of 0x53:0x0400 |
| 0x120-0x148 | P1: probe results `addr<<16 \| PRU response` |
| 0x150-0x164 | P2: PRU response, 16 bytes |
| 0x170-0x1A4 | P3: 3 x {pattern, PRU response of the `[0x11,p]` write, MCU status, MCU value}; the `[0x22]` load responses at 0x230 |
| 0x1B0-0x1E4 | P4: original, response, written bytes, readback 1, restore response, readback 2, flags |
| 0x200-0x220 | C3: step count, control write/read/value/load, 0x44 write/read/value, LEDs-off status |
| 0x3FC | canary `$END` |

## Arms (session order, stops at the first failure)

R0 (R5F-only read of 0x53:0x0400, proves the bus) then, at 100 kHz: P1, P2, C3, P3, P4; then at 400 kHz: P1, P2, P3, P4.
P1 probe by a 1-byte PRU read: 0x61, 0x52, 0x53 must ACK, 0x10 and 0x21 must NACK (a read is harmless on all of them); P2 PRU read of
16 bytes at 0x53:0x0400 equals R0.

- **C3 (new, MCU only, once, 100 kHz, before P3).** The MCU writes `[0x11, 0x5A]`, reads 1 byte and the read must equal 0x5A (the
  control that proves the readback method), writes `[0x22]`. Then it writes `[0x44, 0xA5]`, reads, and only RECORDS the value (not
  gated: it documents the 0x44 readback seen on silicon). It ends with `[0x11,0x00]`, `[0x22]` so the LEDs are off.
- **P3 (changed).** Per pattern 0x55, 0xAA, 0x00: a PRU phase writes `[0x11, pattern]`; the PRU is stopped and both lines released
  (guard-proven) before the MCU opens; the MCU reads 1 byte from 0x61 (must equal the pattern); a second PRU phase writes the 1-byte
  `[0x22]` (must ACK). Six PRU phases and three MCU opens, all through the same guard, so the strict ownership rule is unchanged.
  The readback is now a gate (the old "visual only" allowance is gone). The PRU write path takes the byte count from the CNT word,
  so a 1-byte write needs no firmware change; **UNVERIFIED** that the PRU firmware sends a correct 1-byte write on the wire.
- P4 saves 8 bytes at 0x53:0x0400, writes their complement, MCU readback, PRU restore, MCU readback. P4 never writes before the
  original is saved and always attempts the restore once a write started; the original is also in the block if the app dies in between.
  `0x52` is never written, `0x53` never below `0x0400` (asserted in the host test).

The TPIC2810 sequence is documented only by the SDK example (no datasheet was read). The host test models it (0x11 sets the shift
register, a read returns it, 0x22 loads the outputs, a read after 0x44 returns 0xFF as on silicon); that model is the silicon
observation plus the SDK sequence, not a datasheet.

## Build, test, run

```
# PRU firmware (CCS gmake; imports.mak is local, DEVICE = am263x)
cd examples/pru_i2c/firmware/am263x-lp/icss_m0_pru1_fw/ti-pru-cgt && gmake
# R5F app (the .out is the artifact; the appimage rule needs an SDK tool the 26.01 SDK does not ship)
cd examples/pru_i2c/mcuplus/am263x-lp/r5fss0-0_nortos/ti-arm-clang && gmake pru_i2c.release.out
node bench/test_rnd91.mjs            # host tests (gcc in WSL for the C part)
node bench/test_rnd91_mutants.mjs    # every mutant must be killed
node bench/rnd91_session.mjs --dry-run
node tools/waitrun.mjs --boards am263 --expect-min 12 --status <runs>/rnd91-bench --owner "rnd91-bench (RND-91)" \
    --preflight "node bench/rnd91_session.mjs --dry-run" -- node bench/rnd91_session.mjs --live <fresh tag>
```

`bench/rnd91_images.md5.json` records the md5 of the staged images (`bench/images/`, not committed). A clean rebuild reproduced the md5
four times on this PC; a partial rebuild over stale objects did not, so build clean.

## Not verified (needs the bench)

PRU reachability of MSS_CTRL and the OUTDISABLE polarity; R30/R31 bit = GPIO number; that the R5F image reload per arm (halt, load,
run, no CPU reset) comes up clean; JTAG reads of the block while the core spins; the new P3 sequence (`[0x11,p]` by the PRU, MCU readback, `[0x22]` by the PRU) and C3; the 1-byte PRU write; the 367 kHz waveform and rise
time; EEPROM page-write behaviour of CAT23M01 through the PRU's write-pointer-then-read sequence (the same sequence the upstream
example uses, never run on a device).
