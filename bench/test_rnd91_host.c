/* Host test of the REAL rnd91_bench.h (no copy of its logic): timing arithmetic, PRU config image,
 * bus-ownership guard and arm sequencer against a fake Rnd91_Hal that models two bus owners, the
 * EEPROM and the TPIC2810 and counts contention. Built and run by bench/test_rnd91.mjs with gcc in WSL.
 *   test_rnd91_host <out dir for the block dumps>
 * Expected values are literals worked out by hand (see am263x-lp-bench.md), not taken from the header. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rnd91_bench.h"

static int gFail = 0;
static int gRun = 0;
#define CHECK(cond, name) do { gRun++; if (!(cond)) { gFail++; printf("FAIL %s (line %d)\n", name, __LINE__); } else { printf("ok   %s\n", name); } } while (0)

/* ---- fake hardware ---------------------------------------------------------------------------- */
typedef struct
{
    uint8_t  ee52[0x800], ee53[0x800];
    uint8_t  tpic;                      /* TPIC2810 shift register */
    uint8_t  tpicOut;                   /* TPIC2810 output latch */
    int      tpicAfter44;               /* silicon: a read after 0x44 returns 0xFF, until the next 0x11 */
    int      loads, cmd44, shifts;      /* [0x22] loads, 0x44 writes, 0x11 writes seen */
    uint8_t  outAtLoad[8];              /* output latch after each load */
    uint32_t ptr;                       /* EEPROM address pointer */
    int      pruRunning, mcuOpen;
    int      contention;                /* either owner acting while the other has the bus */
    int      badCalls;                  /* a transfer without an owner */
    int      pruStartCalls, mcuOpenCalls, totalCalls;
    int      writes52, writesLow, writes53;
    int      stuckPru, failOrigRead, failDataWrite, ackAt10, releasedOnlyScl;
    int      dataWriteSeen;
} Fake;

/* TPIC2810 model: 0x11 sets the shift register, a read returns it, 0x22 loads the outputs from it;
 * 0x44 loads the outputs directly and a read after it returns 0xFF (what silicon showed). */
static void tpicWrite(Fake *f, const uint8_t *d, uint32_t n)
{
    if (n == 2U && d[0] == 0x11U) { f->tpic = d[1]; f->tpicAfter44 = 0; f->shifts++; }
    else if (n == 1U && d[0] == 0x22U) { f->tpicOut = f->tpic; if (f->loads < 8) f->outAtLoad[f->loads] = f->tpicOut; f->loads++; }
    else if (n == 2U && d[0] == 0x44U) { f->tpic = d[1]; f->tpicOut = d[1]; f->tpicAfter44 = 1; f->cmd44++; }
}
static uint8_t tpicRead(Fake *f) { return f->tpicAfter44 ? 0xFFU : f->tpic; }

static int fakeAck(Fake *f, uint8_t a)
{
    return a == 0x61 || a == 0x52 || a == 0x53 || (a == 0x10 && f->ackAt10);
}

static int32_t f_pruStart(void *c, uint32_t hz) { Fake *f = c; (void)hz; f->totalCalls++; if (f->mcuOpen) f->contention++; f->pruRunning = 1; f->pruStartCalls++; return 0; }
static int32_t f_pruStop(void *c, uint32_t *ctrl, uint32_t *outc)
{
    Fake *f = c;
    f->totalCalls++;
    if (f->stuckPru) { *ctrl = RND91_CTRL_ENABLE | RND91_CTRL_RUNNING; *outc = 0U; return 0; }
    f->pruRunning = 0;
    *ctrl = 0U;
    *outc = f->releasedOnlyScl ? (1U << RND91_SCL_PIN) : (RND91_PIN_MASK | 0x100U);
    return 0;
}
static int32_t f_pruWrite(void *c, uint8_t a, const uint8_t *d, uint32_t n, uint16_t *resp)
{
    Fake *f = c; uint32_t i;
    f->totalCalls++;
    if (!f->pruRunning) f->badCalls++;
    if (!fakeAck(f, a)) { *resp = ADDRESS_ACKNOWLDEGE_FAILED; return -1; }
    if (a == 0x61) { tpicWrite(f, d, n); *resp = COMMAND_SUCCESS; return 0; }
    if (n >= 2U) f->ptr = ((uint32_t)d[0] << 8) | d[1];
    if (n > 2U)
    {
        if (f->failDataWrite && a == 0x53 && !f->dataWriteSeen) { f->dataWriteSeen = 1; *resp = DATA_ACKNOWLDEGE_FAILED; return -1; }
        for (i = 2U; i < n; i++)
        {
            uint32_t at = f->ptr + (i - 2U);
            if (a == 0x52) { f->writes52++; f->ee52[at] = d[i]; }
            else { f->writes53++; if (at < 0x400U) f->writesLow++; f->ee53[at] = d[i]; }
        }
    }
    *resp = COMMAND_SUCCESS;
    return 0;
}
static int32_t f_pruRead(void *c, uint8_t a, uint8_t *d, uint32_t n, uint16_t *resp)
{
    Fake *f = c; uint32_t i;
    f->totalCalls++;
    if (!f->pruRunning) f->badCalls++;
    if (!fakeAck(f, a)) { *resp = ADDRESS_ACKNOWLDEGE_FAILED; return -1; }
    if (f->failOrigRead && a == 0x53 && n == 8U) { *resp = ADDRESS_ACKNOWLDEGE_FAILED; return -1; }
    for (i = 0U; i < n; i++) d[i] = (a == 0x61) ? tpicRead(f) : (a == 0x52 ? f->ee52 : f->ee53)[(f->ptr + i) & 0x7FFU];
    *resp = COMMAND_SUCCESS;
    return 0;
}
static int32_t f_mcuOpen(void *c) { Fake *f = c; f->totalCalls++; if (f->pruRunning) f->contention++; f->mcuOpen = 1; f->mcuOpenCalls++; return 0; }
static void f_mcuClose(void *c) { Fake *f = c; f->totalCalls++; f->mcuOpen = 0; }
static int32_t f_mcuRead(void *c, uint8_t a, uint8_t *d, uint32_t n)
{
    Fake *f = c; uint32_t i;
    f->totalCalls++;
    if (!f->mcuOpen) f->badCalls++;
    for (i = 0U; i < n; i++) d[i] = tpicRead(f);
    return (a == 0x61) ? 0 : -1;
}
static int32_t f_mcuWrite(void *c, uint8_t a, const uint8_t *d, uint32_t n)
{
    Fake *f = c;
    f->totalCalls++;
    if (!f->mcuOpen) f->badCalls++;
    if (a != 0x61) return -1;
    tpicWrite(f, d, n);
    return 0;
}
static int32_t f_mcuMem(void *c, uint8_t a, uint16_t m, uint8_t *d, uint32_t n)
{
    Fake *f = c; uint32_t i;
    f->totalCalls++;
    if (!f->mcuOpen) f->badCalls++;
    for (i = 0U; i < n; i++) d[i] = (a == 0x52 ? f->ee52 : f->ee53)[(m + i) & 0x7FFU];
    return 0;
}
static void f_sleep(void *c, uint32_t us) { (void)c; (void)us; }

static const Rnd91_Hal gHal = { f_pruStart, f_pruStop, f_pruWrite, f_pruRead, f_mcuOpen, f_mcuClose, f_mcuRead, f_mcuWrite, f_mcuMem, f_sleep };

static uint8_t gBlk[RND91_BLOCK_SIZE] __attribute__((aligned(4)));

static void fakeInit(Fake *f)
{
    uint32_t i;
    memset(f, 0, sizeof(*f));
    for (i = 0U; i < 0x800U; i++) { f->ee52[i] = (uint8_t)(0x30U + i); f->ee53[i] = (uint8_t)(0xA0U + (i * 7U)); }
}

static uint32_t runArm(Fake *f, uint32_t arm, uint32_t hz, uint32_t magic, uint32_t cookie, Rnd91_Run *run)
{
    memset(gBlk, 0, sizeof(gBlk));
    Rnd91_w32(gBlk, RND91_OFF_REQ_MAGIC, magic);
    Rnd91_w32(gBlk, RND91_OFF_REQ_ARM, arm);
    Rnd91_w32(gBlk, RND91_OFF_REQ_HZ, hz);
    Rnd91_w32(gBlk, RND91_OFF_REQ_COOKIE, cookie);
    run->hal = &gHal; run->ctx = f; run->blk = gBlk; run->guard.blk = gBlk; run->guard.owner = RND91_OWNER_NONE; run->busHz = 0U;
    return Rnd91_runRequested(run, 200000000U);
}

static int hasEvent(uint32_t code)
{
    uint32_t n = Rnd91_r32(gBlk, RND91_OFF_EV_COUNT), i;
    for (i = 0U; i < n && i < RND91_EV_MAX; i++) if ((Rnd91_r32(gBlk, RND91_OFF_EV + 4U * i) >> 24) == code) return 1;
    return 0;
}

static void dump(const char *dir, const char *name)
{
    char path[512]; FILE *fp;
    snprintf(path, sizeof(path), "%s/%s.bin", dir, name);
    fp = fopen(path, "wb");
    if (fp) { fwrite(gBlk, 1, sizeof(gBlk), fp); fclose(fp); }
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : ".";
    Fake f; Rnd91_Run run; uint32_t e, hzs[2] = {100000U, 400000U}, k, a;
    char nm[64];

    /* 1. timing arithmetic at 200 MHz, worked by hand:
     *    100 kHz: clks = ceil(200e6/(4*100e3)) = 500; tLOW floor = ceil(200000*680/1e6) = 136 -> 500; inc = 2500 (0x9C4);
     *             SCL = 200e6/(4*500) = 100000; timeout = 25*200000/500 = 10000 ticks
     *    400 kHz: clks = 125 < 136 -> 136; inc = 680 (0x2A8); SCL = 200e6/544 = 367647; timeout = 25*200000/136 = 36764 */
    CHECK(Rnd91_clksBus(200000000U, 100000U) == 500U, "timing: 100k bus clks 500");
    CHECK(Rnd91_clksTlow(200000000U) == 136U, "timing: tLOW floor 136 clks (680 ns)");
    CHECK(Rnd91_iepIncrement(200000000U, 100000U) == 2500U, "timing: 100k IEP increment 2500");
    CHECK(Rnd91_sclHz(200000000U, 100000U) == 100000U, "timing: 100k SCL 100000 Hz");
    CHECK(Rnd91_sclTimeoutTicks(200000000U, 100000U) == 10000U, "timing: 100k SCL timeout 10000 ticks");
    CHECK(Rnd91_clksBus(200000000U, 400000U) == 125U, "timing: 400k bus clks 125");
    CHECK(Rnd91_clksPerTick(200000000U, 400000U) == 136U, "timing: 400k limited by tLOW to 136 clks");
    CHECK(Rnd91_iepIncrement(200000000U, 400000U) == 680U, "timing: 400k IEP increment 680");
    CHECK(Rnd91_sclHz(200000000U, 400000U) == 367647U, "timing: 400k actual SCL 367647 Hz");
    CHECK(Rnd91_sclTimeoutTicks(200000000U, 400000U) == 36764U, "timing: 400k SCL timeout 36764 ticks");
    CHECK(Rnd91_iepIncrement(200000000U, 100000U) == IEP_CMP_INCREMENT_VAL_100KHZ, "timing: matches interface header 100k constant");
    CHECK(Rnd91_iepIncrement(200000000U, 400000U) == IEP_CMP_INCREMENT_VAL_400KHZ, "timing: matches interface header 400k constant");
    CHECK(Rnd91_freqSel(100000U) == 3U && Rnd91_freqSel(400000U) == 2U && Rnd91_freqSel(1000000U) == 0U, "timing: frequency selectors");

    /* 2. pins */
    CHECK(RND91_SCL_PIN == 7U, "pins: SCL on R30/R31 bit 7 (PR0_PRU1_GPIO7, ball A16)");
    CHECK(RND91_SDA_PIN == 17U, "pins: SDA on R30/R31 bit 17 (PR0_PRU1_GPIO17, ball D13)");
    CHECK(RND91_PIN_MASK == 0x00020080U, "pins: mask 0x20080");

    /* 3. PRU config image, literal offsets: instance block at 0x100 */
    {
        static uint8_t d0[0x2000], d1[0x2000];
        memset(d0, 0, sizeof(d0)); memset(d1, 0, sizeof(d1));
        d0[0x08] = 0xFF; d0[0x09] = 0xFF;                               /* the other core's IRQ bits must survive */
        Rnd91_fillPruConfig(d0, d1, 3U, 2500U, 10000U);
        CHECK(d1[0x100 + 0xD8] == 7U, "config: SCL pin byte (instance+0xD8) = 7");
        CHECK(d1[0x100 + 0xD9] == 17U, "config: SDA pin byte (instance+0xD9) = 17");
        CHECK(d1[0x100 + 0xE4] == 1U, "config: instance id = 1 (PRU1)");
        CHECK(*(uint16_t *)(d0 + 0x0C) == 3U && *(uint16_t *)(d0 + 0x0E) == 2500U, "config: freq word 3, IEP increment 2500");
        CHECK(*(uint16_t *)(d1 + 0x100 + 0xE8) == 10000U, "config: SCL timeout ticks");
        CHECK(*(uint32_t *)(d1 + 0x100 + 0xA4) == 0x8413U, "config: CON 0x8413 (enable, master, start, stop, NACK last)");
        CHECK(*(uint16_t *)(d0 + 0x08) == 0xFFFDU, "config: only this instance's IRQ bit (bit 1) cleared");
    }

    /* 4. nominal runs of every arm at both rates */
    for (k = 0; k < 2; k++)
    {
        for (a = RND91_ARM_R0; a <= RND91_ARM_C3; a++)
        {
            fakeInit(&f);
            e = runArm(&f, a, hzs[k], RND91_REQ_MAGIC, 0x52000000U + a, &run);
            snprintf(nm, sizeof(nm), "arm %u @%uk: done, no error", a, hzs[k] / 1000U);
            CHECK(e == 0U && Rnd91_r32(gBlk, RND91_OFF_STATE) == RND91_STATE_DONE, nm);
            snprintf(nm, sizeof(nm), "arm %u @%uk: no bus contention, no stray transfer", a, hzs[k] / 1000U);
            CHECK(f.contention == 0 && f.badCalls == 0 && !f.pruRunning && !f.mcuOpen, nm);
            snprintf(nm, sizeof(nm), "arm %u @%uk: 0x52 never written, 0x53 never below 0x0400", a, hzs[k] / 1000U);
            CHECK(f.writes52 == 0 && f.writesLow == 0, nm);
            snprintf(nm, sizeof(nm), "nominal-%u-%u", a, hzs[k] / 1000U);
            dump(out, nm);
        }
    }
    /* P3: [0x11,p] by the PRU, MCU readback == p, [0x22] by the PRU; outputs end up showing each pattern, never 0x44 */
    fakeInit(&f);
    e = runArm(&f, RND91_ARM_P3, 100000U, RND91_REQ_MAGIC, 21U, &run);
    CHECK(e == 0U && f.shifts == 3 && f.loads == 3 && f.cmd44 == 0, "P3: three [0x11,p] writes, three [0x22] loads, no 0x44");
    CHECK(f.outAtLoad[0] == 0x55U && f.outAtLoad[1] == 0xAAU && f.outAtLoad[2] == 0x00U && f.tpicOut == 0x00U, "P3: outputs 0x55, 0xAA, 0x00 after the loads");
    CHECK(Rnd91_r32(gBlk, RND91_OFF_P3 + 12U) == 0x55U && Rnd91_r32(gBlk, RND91_OFF_P3 + 16U + 12U) == 0xAAU, "P3: MCU readback of the shift register equals the pattern");
    CHECK(Rnd91_r32(gBlk, RND91_OFF_STOP_N) == 6U, "P3: six PRU stops (two phases per pattern)");
    CHECK(f.pruStartCalls == 6 && f.mcuOpenCalls == 3, "P3: six PRU phases, three MCU opens");
    /* C3: control sequence, then 0x44 (readback 0xFF on silicon, recorded), then LEDs off */
    fakeInit(&f);
    e = runArm(&f, RND91_ARM_C3, 100000U, RND91_REQ_MAGIC, 22U, &run);
    CHECK(e == 0U && f.pruStartCalls == 0 && f.mcuOpenCalls == 1, "C3: MCU only, one driver open, no PRU start");
    CHECK(Rnd91_r32(gBlk, RND91_OFF_C3_RD1_VAL) == 0x5AU && Rnd91_r32(gBlk, RND91_OFF_C3_RD2_VAL) == 0xFFU, "C3: control readback 0x5A, 0x44 readback 0xFF (recorded)");
    CHECK(f.cmd44 == 1 && f.loads == 2 && f.tpicOut == 0x00U && f.tpic == 0x00U && Rnd91_r32(gBlk, RND91_OFF_C3_N) == 8U, "C3: ends with the LEDs off (0x11,0x00 then 0x22)");
    CHECK(Rnd91_r32(gBlk, RND91_OFF_C3_OFF) == 0U, "C3: off sequence status 0");
    /* a device that ACKs 0x10 where nothing should be: recorded as such, for the session gate test */
    fakeInit(&f); f.ackAt10 = 1;
    e = runArm(&f, RND91_ARM_P1, 100000U, RND91_REQ_MAGIC, 0x52000000U + RND91_ARM_P1, &run);
    dump(out, "ack10-2-100");
    fakeInit(&f);
    e = runArm(&f, RND91_ARM_P4, 100000U, RND91_REQ_MAGIC, 7U, &run);
    CHECK(f.ee53[0x400] == (uint8_t)(0xA0U + 0x400U * 7U) && f.writes53 == 16, "P4: EEPROM restored to the original after write+restore (16 byte-writes)");
    CHECK(Rnd91_r32(gBlk, RND91_OFF_P4_FLAGS) == (RND91_P4_F_WRITE_ISSUED | RND91_P4_F_RESTORE_ISSUED), "P4: write and restore both flagged");
    CHECK(Rnd91_r32(gBlk, RND91_OFF_SCL_HZ) == 100000U && Rnd91_r32(gBlk, RND91_OFF_IEP_INC) == 2500U, "P4: timing words recorded in the block");

    /* 5. a PRU that does not stop: the MCU driver must never be opened */
    for (a = RND91_ARM_P3; a <= RND91_ARM_P4; a++)
    {
        fakeInit(&f); f.stuckPru = 1;
        e = runArm(&f, a, 100000U, RND91_REQ_MAGIC, 9U, &run);
        snprintf(nm, sizeof(nm), "stuck PRU, arm %u: MCU I2C1 never opened", a);
        CHECK(f.mcuOpenCalls == 0 && f.contention == 0, nm);
        snprintf(nm, sizeof(nm), "stuck PRU, arm %u: abort, NOT_RELEASED, REFUSED logged", a);
        CHECK(e == RND91_ERR_NOT_RELEASED && Rnd91_r32(gBlk, RND91_OFF_STATE) == RND91_STATE_ABORT && hasEvent(RND91_EV_REFUSED), nm);
        snprintf(nm, sizeof(nm), "stuck-%u", a);
        dump(out, nm);
    }
    /* one line released, the other not: also not "released" */
    fakeInit(&f); f.releasedOnlyScl = 1;
    e = runArm(&f, RND91_ARM_P3, 100000U, RND91_REQ_MAGIC, 9U, &run);
    CHECK(f.mcuOpenCalls == 0 && e == RND91_ERR_NOT_RELEASED, "only SCL released (SDA still driven): MCU I2C1 never opened");
    CHECK(Rnd91_linesReleased(0U, RND91_PIN_MASK) && !Rnd91_linesReleased(RND91_CTRL_ENABLE, RND91_PIN_MASK) &&
          !Rnd91_linesReleased(RND91_CTRL_RUNNING, RND91_PIN_MASK) && !Rnd91_linesReleased(0U, 1U << RND91_SDA_PIN) &&
          !Rnd91_linesReleased(0U, 1U << RND91_SCL_PIN), "linesReleased: needs core stopped and BOTH bits");

    /* 6. guard directly */
    {
        Rnd91_Guard g; g.blk = gBlk; g.owner = RND91_OWNER_NONE; memset(gBlk, 0, sizeof(gBlk));
        CHECK(Rnd91_guardPruStart(&g) == 0U, "guard: PRU start from NONE");
        CHECK(Rnd91_guardMcuOpen(&g) == RND91_ERR_GUARD_OWNED, "guard: MCU open refused while PRU owns the bus");
        CHECK(Rnd91_guardPruStart(&g) == RND91_ERR_GUARD_OWNED, "guard: second PRU start refused");
        CHECK(Rnd91_guardPruStopped(&g, RND91_CTRL_ENABLE, RND91_PIN_MASK) == RND91_ERR_NOT_RELEASED && g.owner == RND91_OWNER_PRU, "guard: PRU still owner if CONTROL says running");
        CHECK(Rnd91_guardPruStopped(&g, 0U, RND91_PIN_MASK) == 0U && g.owner == RND91_OWNER_NONE, "guard: PRU stopped and released frees the bus");
        CHECK(Rnd91_guardMcuOpen(&g) == 0U, "guard: MCU open after release");
        CHECK(Rnd91_guardPruStart(&g) == RND91_ERR_GUARD_OWNED, "guard: PRU start refused while MCU is open");
    }

    /* 7. P4 never writes before the original is saved */
    fakeInit(&f); f.failOrigRead = 1;
    e = runArm(&f, RND91_ARM_P4, 100000U, RND91_REQ_MAGIC, 11U, &run);
    CHECK(f.writes53 == 0 && f.writes52 == 0 && e == RND91_ERR_ORIG_NOT_SAVED, "P4: original not saved -> nothing written");
    CHECK((Rnd91_r32(gBlk, RND91_OFF_P4_FLAGS) & RND91_P4_F_WRITE_ISSUED) == 0U, "P4: write not flagged when not issued");
    /* 8. P4 write fails after it was started: the restore is still attempted */
    fakeInit(&f); f.failDataWrite = 1;
    e = runArm(&f, RND91_ARM_P4, 100000U, RND91_REQ_MAGIC, 12U, &run);
    CHECK((Rnd91_r32(gBlk, RND91_OFF_P4_FLAGS) & RND91_P4_F_RESTORE_ISSUED) != 0U && e != 0U, "P4: failed write -> restore still issued, arm failed");

    /* 9. bad requests touch no hardware */
    fakeInit(&f);
    e = runArm(&f, RND91_ARM_P1, 100000U, 0U, 1U, &run);
    CHECK(e == RND91_ERR_BAD_REQUEST && f.totalCalls == 0, "request without magic: refused, no HAL call");
    e = runArm(&f, 0U, 100000U, RND91_REQ_MAGIC, 1U, &run);
    CHECK(e == RND91_ERR_BAD_REQUEST && f.totalCalls == 0, "arm 0: refused, no HAL call");
    e = runArm(&f, RND91_ARM_C3 + 1U, 100000U, RND91_REQ_MAGIC, 1U, &run);
    CHECK(e == RND91_ERR_BAD_REQUEST && f.totalCalls == 0, "arm 7: refused, no HAL call");
    e = runArm(&f, RND91_ARM_P4, 1000000U, RND91_REQ_MAGIC, 1U, &run);
    CHECK(e == RND91_ERR_BAD_REQUEST && f.totalCalls == 0, "1 MHz: refused, no HAL call");

    printf("%d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
