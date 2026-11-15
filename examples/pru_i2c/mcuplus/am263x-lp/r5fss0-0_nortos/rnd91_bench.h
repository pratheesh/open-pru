/* File: rnd91_bench.h */
/*
 *  Hardware-independent logic of the LP-AM263 PRU I2C bench app (RND-91):
 *  the result block layout, the timing arithmetic, the PRU configuration
 *  image, the bus-ownership guard and the arm sequencer. main.c supplies the
 *  hardware through Rnd91_Hal; the host tests (bench/test_rnd91_host.sh)
 *  compile this same header against a fake Rnd91_Hal.
 *
 *  Nothing in here has run on silicon.
 *
 *  Why a guard: the PRU (R30/R31 bits 7 and 17) and the MCU I2C1 controller
 *  (pads C8/D7) are wired to the same two bus wires on the bench. If both
 *  could drive the bus at once, a PRU low against an ACKing device or the
 *  MCU controller would be a contention. So the bus has exactly one owner at
 *  a time and every change of owner goes through Rnd91_Guard, which refuses
 *  to hand the bus to the MCU until the PRU is proven stopped with both lines
 *  released (Rnd91_linesReleased).
 */
#ifndef RND91_BENCH_H_
#define RND91_BENCH_H_

#include <stdint.h>
#include "pru_i2c_interface.h"

/* ========================================================================== */
/*                               Fixed addresses                              */
/* ========================================================================== */

/* Result block: MSRAM, the lab's scratch address (tilab lab.json
 * boards.am263.msramScratch). Read by the session over JTAG. */
#define RND91_BLOCK_ADDR            (0x70170000U)
#define RND91_BLOCK_SIZE            (0x400U)

/* ========================================================================== */
/*                         Result block layout (u32 words)                    */
/*  One #define per word or array, "OFF" = byte offset from RND91_BLOCK_ADDR. */
/*  bench/rnd91_lib.mjs reads these lines, so keep the "#define NAME (0x..U)" */
/*  form.                                                                     */
/* ========================================================================== */

#define RND91_MAGIC                 (0x52393142U)   /* 'R91B' */
#define RND91_REQ_MAGIC             (0x52393151U)   /* 'R91Q' */
#define RND91_VERSION               (1U)
#define RND91_CANARY                (0x444E4524U)   /* '$END' */

/* header */
#define RND91_OFF_MAGIC             (0x000U)
#define RND91_OFF_VERSION           (0x004U)
#define RND91_OFF_STATE             (0x008U)        /* RND91_STATE_* */
#define RND91_OFF_STAGE             (0x00CU)        /* counts milestones: where a hang stopped */
/* request, written by the session before the core runs */
#define RND91_OFF_REQ_MAGIC         (0x010U)
#define RND91_OFF_REQ_ARM           (0x014U)        /* RND91_ARM_* */
#define RND91_OFF_REQ_HZ            (0x018U)        /* 100000 or 400000 */
#define RND91_OFF_REQ_COOKIE        (0x01CU)        /* echoed back, tells this run from a stale block */
/* status and the timing the app derived */
#define RND91_OFF_ERR               (0x020U)        /* RND91_ERR_*, 0 = none */
#define RND91_OFF_IEP_INC           (0x024U)        /* IEP compare increment per state tick */
#define RND91_OFF_TICK_CLKS         (0x028U)        /* IEP clocks per state tick */
#define RND91_OFF_SCL_TMO_TICKS     (0x02CU)
#define RND91_OFF_FREQ_SEL          (0x030U)        /* ICSS_I2C_xxxKHZ_FREQ */
#define RND91_OFF_SCL_HZ            (0x034U)        /* the SCL rate the tick gives, Hz */
/* event log: word = (code << 24) | arg */
#define RND91_OFF_EV_COUNT          (0x040U)        /* total events, may exceed RND91_EV_MAX */
#define RND91_OFF_EV                (0x044U)        /* RND91_EV_MAX words */
#define RND91_EV_MAX                (32U)
/* PRU CONTROL register and OUT_CTRL as read right after each PRU stop */
#define RND91_OFF_STOP_N            (0x0C4U)
#define RND91_OFF_STOP_CTRL         (0x0C8U)        /* RND91_STOP_MAX words */
#define RND91_OFF_STOP_OUTCTRL      (0x0D8U)        /* RND91_STOP_MAX words */
#define RND91_STOP_MAX              (4U)
/* stops 4..7 (P3 has two PRU phases per pattern, six stops): same pair of words, in the free space after P4 */
#define RND91_OFF_STOP_X_CTRL       (0x240U)        /* RND91_STOP_X_MAX words */
#define RND91_OFF_STOP_X_OUTCTRL    (0x250U)        /* RND91_STOP_X_MAX words */
#define RND91_STOP_X_MAX            (4U)
/* R0: MCU read of 16 bytes at 0x53:0x0400 */
#define RND91_OFF_R0_STATUS         (0x100U)        /* SystemP status (0 = ok) */
#define RND91_OFF_R0_DATA           (0x104U)        /* 16 bytes */
/* P1: probe. word = (addr7 << 16) | PRU response */
#define RND91_OFF_PROBE_N           (0x120U)
#define RND91_OFF_PROBE             (0x124U)        /* RND91_PROBE_MAX words */
#define RND91_PROBE_MAX             (8U)
/* P2: PRU read of 16 bytes at 0x53:0x0400 */
#define RND91_OFF_P2_RESP           (0x150U)        /* PRU response of the data read */
#define RND91_OFF_P2_DATA           (0x154U)        /* 16 bytes */
/* P3: three entries of 4 words {pattern, PRU response of the [0x11,pattern] write, MCU status, MCU value}.
 * The PRU response of the [0x22] load write of entry i is RND91_OFF_P3_LOAD + 4 * i. */
#define RND91_OFF_P3_N              (0x170U)
#define RND91_OFF_P3                (0x174U)
#define RND91_P3_MAX                (3U)
#define RND91_P3_STRIDE             (0x10U)
/* P4: EEPROM write, 8 bytes at 0x53:0x0400 */
#define RND91_OFF_P4_ORIG_RESP      (0x1B0U)        /* PRU response of the save of the original */
#define RND91_OFF_P4_ORIG           (0x1B4U)        /* 8 bytes */
#define RND91_OFF_P4_WR_RESP        (0x1BCU)        /* PRU response of the write */
#define RND91_OFF_P4_NEW            (0x1C0U)        /* 8 bytes written */
#define RND91_OFF_P4_RB1_STATUS     (0x1C8U)        /* MCU status of readback 1 */
#define RND91_OFF_P4_RB1            (0x1CCU)        /* 8 bytes */
#define RND91_OFF_P4_RST_RESP       (0x1D4U)        /* PRU response of the restore */
#define RND91_OFF_P4_RB2_STATUS     (0x1D8U)        /* MCU status of readback 2 */
#define RND91_OFF_P4_RB2            (0x1DCU)        /* 8 bytes */
#define RND91_OFF_P4_FLAGS          (0x1E4U)        /* RND91_P4_F_* */
#define RND91_P4_F_WRITE_ISSUED     (0x1U)          /* a write to the EEPROM was started */
#define RND91_P4_F_RESTORE_ISSUED   (0x2U)          /* the restore was started */
#define RND91_OFF_P3_LOAD           (0x230U)        /* RND91_P3_MAX words */
/* C3: MCU-only TPIC2810 control. Every word is a SystemP status (0 = ok) unless noted. */
#define RND91_OFF_C3_N              (0x200U)        /* steps completed, 0..8 */
#define RND91_OFF_C3_WR1            (0x204U)        /* write [0x11, 0x5A] */
#define RND91_OFF_C3_RD1            (0x208U)        /* read 1 byte */
#define RND91_OFF_C3_RD1_VAL        (0x20CU)        /* the byte, gated == 0x5A */
#define RND91_OFF_C3_LD1            (0x210U)        /* write [0x22] */
#define RND91_OFF_C3_WR2            (0x214U)        /* write [0x44, 0xA5] */
#define RND91_OFF_C3_RD2            (0x218U)        /* read 1 byte */
#define RND91_OFF_C3_RD2_VAL        (0x21CU)        /* the byte after 0x44: recorded only, never gated */
#define RND91_OFF_C3_OFF            (0x220U)        /* end: write [0x11,0x00] then [0x22]; first nonzero status */
#define RND91_OFF_CANARY            (0x3FCU)

#define RND91_STATE_CLEARED         (0U)
#define RND91_STATE_RUNNING         (1U)
#define RND91_STATE_DONE            (2U)
#define RND91_STATE_ABORT           (3U)

/* ========================================================================== */
/*                          Arms, errors, events, targets                     */
/* ========================================================================== */

#define RND91_ARM_R0                (1U)
#define RND91_ARM_P1                (2U)
#define RND91_ARM_P2                (3U)
#define RND91_ARM_P3                (4U)
#define RND91_ARM_P4                (5U)
#define RND91_ARM_C3                (6U)        /* MCU-only TPIC2810 readback control, runs before P3 */

#define RND91_ERR_NONE              (0U)
#define RND91_ERR_BAD_REQUEST       (1U)    /* request magic, arm or frequency not valid */
#define RND91_ERR_GUARD_OWNED       (2U)    /* bus handover refused: the other owner has it */
#define RND91_ERR_NOT_RELEASED      (3U)    /* PRU not proven stopped with both lines released */
#define RND91_ERR_PRU_START         (4U)
#define RND91_ERR_PRU_XFER          (5U)    /* a PRU transfer failed (the response word says how) */
#define RND91_ERR_MCU_OPEN          (6U)
#define RND91_ERR_MCU_XFER          (7U)
#define RND91_ERR_ORIG_NOT_SAVED    (8U)    /* P4 refuses to write before the original is saved */

#define RND91_EV_START              (1U)
#define RND91_EV_ARM_BEGIN          (2U)    /* arg = arm */
#define RND91_EV_PRU_START          (3U)    /* the PRU owns the bus from here */
#define RND91_EV_PRU_STOPPED        (4U)    /* arg = PRU CONTROL register */
#define RND91_EV_LINES_RELEASED     (5U)    /* arg = OUT_CTRL & pin mask */
#define RND91_EV_MCU_OPEN           (6U)    /* the MCU I2C1 controller owns the bus from here */
#define RND91_EV_MCU_CLOSE          (7U)
#define RND91_EV_ARM_END            (8U)    /* arg = error code */
#define RND91_EV_REFUSED            (9U)    /* guard refused a handover, arg = error code */

/* Targets (7-bit addresses) */
#define RND91_ADDR_TPIC2810         (0x61U) /* LED driver */
#define RND91_ADDR_EEPROM_ID        (0x52U) /* board ID: never written */
#define RND91_ADDR_EEPROM           (0x53U) /* upper 64 KB half: written only at >= 0x0400 */
#define RND91_EEPROM_TEST_OFFSET    (0x0400U)
#define RND91_EEPROM_WRITE_MIN      (0x0400U)
#define RND91_EEPROM_WRITE_CYCLE_US (10000U)
#define RND91_TPIC_CMD_WR_NOW       (0x44U) /* write shift register and load the outputs */
#define RND91_TPIC_CMD_SHIFT        (0x11U) /* write the shift register (SDK i2c_led_read_write_lld) */
#define RND91_TPIC_CMD_LOAD         (0x22U) /* load the outputs from the shift register */
#define RND91_C3_PATTERN_CTRL       (0x5AU) /* C3 control pattern, gated on readback */
#define RND91_C3_PATTERN_DOC        (0xA5U) /* C3 0x44 pattern, readback recorded only */

/* ========================================================================== */
/*                                  Pins                                      */
/* ========================================================================== */

/* R30 (output enable control) / R31 (input) bit numbers = the PR0_PRU1_GPIOn
 * number. J8-74 (ball A16) -> J5-49 I2C1_SCL, J8-73 (ball D13) -> J5-50 I2C1_SDA. */
#define RND91_SCL_PIN               (7U)
#define RND91_SDA_PIN               (17U)
#define RND91_PIN_MASK              ((1U << RND91_SCL_PIN) | (1U << RND91_SDA_PIN))
#define RND91_PRU_CORE              (1U)    /* PRU1 */

/* PDSP CONTROL register: bit 1 PDSP_ENABLE, bit 15 PDSP_STATE (running) */
#define RND91_CTRL_ENABLE           (0x00000002U)
#define RND91_CTRL_RUNNING          (0x00008000U)

/* ========================================================================== */
/*                               Timing arithmetic                            */
/* ========================================================================== */

/* The state machine advances once per IEP compare event, 4 events per SCL
 * period, SCL low for 2 of them. The IEP counts RND91_IEP_DEFAULT_INC per IEP
 * clock. The tick is at least 1/(4*bus Hz) and at least RND91_MIN_TICK_NS so
 * that SCL low meets the Fast-mode tLOW >= 1.3 us with margin for the cycles
 * the edges move by. At 400 kHz the second limit wins (367.6 kHz). */
#define RND91_IEP_DEFAULT_INC       (5U)
#define RND91_MIN_TICK_NS           (680U)
#define RND91_SCL_TIMEOUT_MS        (25U)

static inline uint32_t Rnd91_clksBus(uint32_t iepClkHz, uint32_t busHz)
{
    return (iepClkHz + (4U * busHz) - 1U) / (4U * busHz);
}

static inline uint32_t Rnd91_clksTlow(uint32_t iepClkHz)
{
    return ((iepClkHz / 1000U) * RND91_MIN_TICK_NS + 999999U) / 1000000U;
}

static inline uint32_t Rnd91_clksPerTick(uint32_t iepClkHz, uint32_t busHz)
{
    uint32_t bus = Rnd91_clksBus(iepClkHz, busHz);
    uint32_t low = Rnd91_clksTlow(iepClkHz);

    return (bus > low) ? bus : low;
}

static inline uint32_t Rnd91_iepIncrement(uint32_t iepClkHz, uint32_t busHz)
{
    return Rnd91_clksPerTick(iepClkHz, busHz) * RND91_IEP_DEFAULT_INC;
}

/* SCL stretching limit in state machine ticks (u16 in the PRU instance). */
static inline uint32_t Rnd91_sclTimeoutTicks(uint32_t iepClkHz, uint32_t busHz)
{
    uint32_t ticks = (RND91_SCL_TIMEOUT_MS * (iepClkHz / 1000U)) / Rnd91_clksPerTick(iepClkHz, busHz);

    return (ticks > 0xFFFFU) ? 0xFFFFU : ticks;
}

/* The SCL rate the chosen tick gives */
static inline uint32_t Rnd91_sclHz(uint32_t iepClkHz, uint32_t busHz)
{
    return iepClkHz / (4U * Rnd91_clksPerTick(iepClkHz, busHz));
}

/* Firmware frequency selector for a nominal bus rate, 0 if not supported */
static inline uint32_t Rnd91_freqSel(uint32_t busHz)
{
    uint32_t sel = 0U;

    if (busHz == 100000U)
    {
        sel = ICSS_I2C_100KHZ_FREQ;
    }
    else if (busHz == 400000U)
    {
        sel = ICSS_I2C_400KHZ_FREQ;
    }
    return sel;
}

/* ========================================================================== */
/*                         Raw memory access (block, DMEM)                    */
/* ========================================================================== */

static inline void Rnd91_w8(volatile uint8_t *base, uint32_t off, uint8_t v)
{
    *(volatile uint8_t *)(base + off) = v;
}

static inline void Rnd91_w16(volatile uint8_t *base, uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)(base + off) = v;
}

static inline void Rnd91_w32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static inline uint32_t Rnd91_r32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline uint16_t Rnd91_r16(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint16_t *)(base + off);
}

static inline void Rnd91_putBytes(volatile uint8_t *base, uint32_t off, const uint8_t *src, uint32_t n)
{
    uint32_t i;

    for (i = 0U; i < n; i++)
    {
        base[off + i] = src[i];
    }
}

/* ========================================================================== */
/*                         PRU configuration image                            */
/* ========================================================================== */

/* Write the PRU I2C configuration into the data RAM, with the offsets of
 * pru_i2c_interface.h (the same header the firmware is built from).
 * dmem0 = PRU0 data RAM (the two global words), dmem1 = PRU1 data RAM (the
 * instance block). Call with the core disabled. */
static inline void Rnd91_fillPruConfig(volatile uint8_t *dmem0, volatile uint8_t *dmem1,
                                       uint32_t freqSel, uint32_t iepInc, uint32_t sclTmoTicks)
{
    uint32_t con;
    uint16_t irq;

    /* Frequency selector and IEP increment, shared words in DMEM0 */
    Rnd91_w16(dmem0, I2C_BUS_FREQUENCY_OFFSET, (uint16_t)freqSel);
    Rnd91_w16(dmem0, I2C_BUS_FREQUENCY_OFFSET + 2U, (uint16_t)iepInc);
    /* clear only this instance's bit of the shared IRQ word */
    irq = Rnd91_r16(dmem0, IRQ_COMMON_REGISTER_OFFSET);
    irq = (uint16_t)(irq & ~(1U << RND91_PRU_CORE));
    Rnd91_w16(dmem0, IRQ_COMMON_REGISTER_OFFSET, irq);

    /* instance 0 block in the data RAM of the core that runs the firmware */
    Rnd91_w8(dmem1, ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_BUF_OFFSET, 0xFFU);
    Rnd91_w8(dmem1, ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_BUF_OFFSET + 1U, 0xFFU);
    Rnd91_w8(dmem1, ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_PRU_PIN_OFFSET, (uint8_t)RND91_SCL_PIN);
    Rnd91_w8(dmem1, ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_PRU_PIN_OFFSET + 1U, (uint8_t)RND91_SDA_PIN);
    Rnd91_w8(dmem1, ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_PRU_INST_ID_OFFSET, (uint8_t)RND91_PRU_CORE);
    Rnd91_w16(dmem1, ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_SCL_TIMEOUT_OFFSET, (uint16_t)sclTmoTicks);
    con = (1U << ICSS_I2C_MODULE_ENABLE_BIT) | (1U << ICSS_I2C_MASTER_SLAVE_MODE_BIT) |
          (1U << ICSS_I2C_START_BIT) | (1U << ICSS_I2C_STOP_BIT) | (1U << ICSS_I2C_RECIEVE_NACK_BIT);
    Rnd91_w32(dmem1, ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_CON_OFFSET, con);
}

/* ========================================================================== */
/*                          Events and the bus guard                          */
/* ========================================================================== */

static inline void Rnd91_event(volatile uint8_t *blk, uint32_t code, uint32_t arg)
{
    uint32_t n = Rnd91_r32(blk, RND91_OFF_EV_COUNT);

    if (n < RND91_EV_MAX)
    {
        Rnd91_w32(blk, RND91_OFF_EV + (4U * n), (code << 24) | (arg & 0x00FFFFFFU));
    }
    Rnd91_w32(blk, RND91_OFF_EV_COUNT, n + 1U);
}

static inline void Rnd91_stage(volatile uint8_t *blk)
{
    Rnd91_w32(blk, RND91_OFF_STAGE, Rnd91_r32(blk, RND91_OFF_STAGE) + 1U);
}

/* True only if the PRU core is disabled and not running, and both SCL and SDA
 * have their output driver disabled in OUT_CTRL (bit = 1: released). */
static inline int Rnd91_linesReleased(uint32_t ctrlReg, uint32_t outCtrl)
{
    return ((ctrlReg & (RND91_CTRL_ENABLE | RND91_CTRL_RUNNING)) == 0U) &&
           ((outCtrl & RND91_PIN_MASK) == RND91_PIN_MASK);
}

#define RND91_OWNER_NONE            (0U)
#define RND91_OWNER_PRU             (1U)
#define RND91_OWNER_MCU             (2U)

typedef struct
{
    volatile uint8_t *blk;
    uint32_t          owner;
} Rnd91_Guard;

static inline uint32_t Rnd91_guardRefuse(Rnd91_Guard *g, uint32_t err)
{
    Rnd91_event(g->blk, RND91_EV_REFUSED, err);
    return err;
}

/* The PRU may start only while nobody owns the bus. */
static inline uint32_t Rnd91_guardPruStart(Rnd91_Guard *g)
{
    if (g->owner != RND91_OWNER_NONE)
    {
        return Rnd91_guardRefuse(g, RND91_ERR_GUARD_OWNED);
    }
    g->owner = RND91_OWNER_PRU;
    Rnd91_event(g->blk, RND91_EV_PRU_START, 0U);
    return RND91_ERR_NONE;
}

/* The PRU gives the bus back only when it is proven stopped with both lines
 * released. If not, the PRU stays the owner and the MCU can never open. */
static inline uint32_t Rnd91_guardPruStopped(Rnd91_Guard *g, uint32_t ctrlReg, uint32_t outCtrl)
{
    uint32_t n = Rnd91_r32(g->blk, RND91_OFF_STOP_N);

    if (n < RND91_STOP_MAX)
    {
        Rnd91_w32(g->blk, RND91_OFF_STOP_CTRL + (4U * n), ctrlReg);
        Rnd91_w32(g->blk, RND91_OFF_STOP_OUTCTRL + (4U * n), outCtrl);
    }
    else if (n < (RND91_STOP_MAX + RND91_STOP_X_MAX))
    {
        Rnd91_w32(g->blk, RND91_OFF_STOP_X_CTRL + (4U * (n - RND91_STOP_MAX)), ctrlReg);
        Rnd91_w32(g->blk, RND91_OFF_STOP_X_OUTCTRL + (4U * (n - RND91_STOP_MAX)), outCtrl);
    }
    Rnd91_w32(g->blk, RND91_OFF_STOP_N, n + 1U);
    Rnd91_event(g->blk, RND91_EV_PRU_STOPPED, ctrlReg);
    if (!Rnd91_linesReleased(ctrlReg, outCtrl))
    {
        return Rnd91_guardRefuse(g, RND91_ERR_NOT_RELEASED);
    }
    Rnd91_event(g->blk, RND91_EV_LINES_RELEASED, outCtrl & RND91_PIN_MASK);
    g->owner = RND91_OWNER_NONE;
    return RND91_ERR_NONE;
}

/* The MCU I2C1 controller may open only while nobody owns the bus. */
static inline uint32_t Rnd91_guardMcuOpen(Rnd91_Guard *g)
{
    if (g->owner != RND91_OWNER_NONE)
    {
        return Rnd91_guardRefuse(g, RND91_ERR_GUARD_OWNED);
    }
    g->owner = RND91_OWNER_MCU;
    Rnd91_event(g->blk, RND91_EV_MCU_OPEN, 0U);
    return RND91_ERR_NONE;
}

static inline void Rnd91_guardMcuClose(Rnd91_Guard *g)
{
    Rnd91_event(g->blk, RND91_EV_MCU_CLOSE, 0U);
    g->owner = RND91_OWNER_NONE;
}

/* ========================================================================== */
/*                              Hardware interface                            */
/* ========================================================================== */

/* All functions return 0 on success. PRU transfers also return the raw PRU
 * response word in *resp (COMMAND_SUCCESS, ADDRESS_ACKNOWLDEGE_FAILED, ...). */
typedef struct
{
    /* start PRU1 with the configuration for busHz (pinmux/MSS_CTRL unlock,
     * config image, load, run, setup command) */
    int32_t (*pruStart)(void *ctx, uint32_t busHz);
    /* disable PRU1, release both lines in OUT_CTRL, report what was read back */
    int32_t (*pruStopRelease)(void *ctx, uint32_t *ctrlReg, uint32_t *outCtrl);
    int32_t (*pruWrite)(void *ctx, uint8_t addr7, const uint8_t *data, uint32_t len, uint16_t *resp);
    int32_t (*pruRead)(void *ctx, uint8_t addr7, uint8_t *data, uint32_t len, uint16_t *resp);
    /* MCU I2C1 driver */
    int32_t (*mcuOpen)(void *ctx);
    void    (*mcuClose)(void *ctx);
    int32_t (*mcuRead)(void *ctx, uint8_t addr7, uint8_t *data, uint32_t len);
    int32_t (*mcuWrite)(void *ctx, uint8_t addr7, const uint8_t *data, uint32_t len);
    int32_t (*mcuMemRead)(void *ctx, uint8_t addr7, uint16_t memAddr, uint8_t *data, uint32_t len);
    void    (*sleepUs)(void *ctx, uint32_t us);
} Rnd91_Hal;

typedef struct
{
    const Rnd91_Hal  *hal;
    void             *ctx;
    volatile uint8_t *blk;
    Rnd91_Guard       guard;
    uint32_t          busHz;
} Rnd91_Run;

/* ========================================================================== */
/*                               Arm sequencer                                */
/* ========================================================================== */

static inline uint32_t Rnd91_fail(Rnd91_Run *r, uint32_t err)
{
    if (Rnd91_r32(r->blk, RND91_OFF_ERR) == RND91_ERR_NONE)
    {
        Rnd91_w32(r->blk, RND91_OFF_ERR, err);
    }
    return err;
}

/* One PRU phase: start, then body, then stop. The stop and release is always
 * attempted, and its proof decides whether the bus is free again. */
static inline uint32_t Rnd91_pruBegin(Rnd91_Run *r)
{
    uint32_t err = Rnd91_guardPruStart(&r->guard);

    if (err != RND91_ERR_NONE)
    {
        return Rnd91_fail(r, err);
    }
    Rnd91_stage(r->blk);
    if (r->hal->pruStart(r->ctx, r->busHz) != 0)
    {
        return Rnd91_fail(r, RND91_ERR_PRU_START);
    }
    return RND91_ERR_NONE;
}

static inline uint32_t Rnd91_pruEnd(Rnd91_Run *r)
{
    uint32_t ctrl = 0xFFFFFFFFU;
    uint32_t outc = 0U;
    uint32_t err;

    if (r->guard.owner != RND91_OWNER_PRU)
    {
        return Rnd91_fail(r, RND91_ERR_GUARD_OWNED);    /* nothing of ours to stop */
    }
    (void)r->hal->pruStopRelease(r->ctx, &ctrl, &outc);
    Rnd91_stage(r->blk);
    err = Rnd91_guardPruStopped(&r->guard, ctrl, outc);
    return (err == RND91_ERR_NONE) ? err : Rnd91_fail(r, err);
}

/* PRU: set the memory pointer, then read len bytes (EEPROM style) */
static inline int32_t Rnd91_pruMemRead(Rnd91_Run *r, uint8_t addr7, uint16_t memAddr,
                                       uint8_t *data, uint32_t len, uint16_t *resp)
{
    uint8_t ptr[2];
    int32_t st;

    ptr[0] = (uint8_t)(memAddr >> 8);
    ptr[1] = (uint8_t)(memAddr & 0xFFU);
    st = r->hal->pruWrite(r->ctx, addr7, ptr, 2U, resp);
    if (st != 0)
    {
        return st;
    }
    return r->hal->pruRead(r->ctx, addr7, data, len, resp);
}

/* MCU phase: open, one read, close. */
static inline uint32_t Rnd91_mcuBegin(Rnd91_Run *r)
{
    uint32_t err = Rnd91_guardMcuOpen(&r->guard);

    if (err != RND91_ERR_NONE)
    {
        return Rnd91_fail(r, err);
    }
    Rnd91_stage(r->blk);
    if (r->hal->mcuOpen(r->ctx) != 0)
    {
        Rnd91_guardMcuClose(&r->guard);
        return Rnd91_fail(r, RND91_ERR_MCU_OPEN);
    }
    return RND91_ERR_NONE;
}

static inline void Rnd91_mcuEnd(Rnd91_Run *r)
{
    r->hal->mcuClose(r->ctx);
    Rnd91_guardMcuClose(&r->guard);
}

static inline uint32_t Rnd91_armR0(Rnd91_Run *r)
{
    uint8_t buf[16] = {0};
    int32_t st;
    uint32_t err = Rnd91_mcuBegin(r);

    if (err != RND91_ERR_NONE)
    {
        return err;
    }
    st = r->hal->mcuMemRead(r->ctx, RND91_ADDR_EEPROM, RND91_EEPROM_TEST_OFFSET, buf, sizeof(buf));
    Rnd91_mcuEnd(r);
    Rnd91_w32(r->blk, RND91_OFF_R0_STATUS, (uint32_t)st);
    Rnd91_putBytes(r->blk, RND91_OFF_R0_DATA, buf, sizeof(buf));
    return (st == 0) ? RND91_ERR_NONE : Rnd91_fail(r, RND91_ERR_MCU_XFER);
}

/* P1: read one byte from each address. A read is harmless on all of them
 * (EEPROM current-address read, TPIC2810 shift register). */
static inline uint32_t Rnd91_armP1(Rnd91_Run *r)
{
    static const uint8_t addrs[5] = {RND91_ADDR_TPIC2810, RND91_ADDR_EEPROM_ID, RND91_ADDR_EEPROM, 0x10U, 0x21U};
    uint32_t err = Rnd91_pruBegin(r);
    uint32_t i;
    uint32_t endErr;

    if (err == RND91_ERR_NONE)
    {
        for (i = 0U; i < 5U; i++)
        {
            uint8_t b = 0U;
            uint16_t resp = 0U;

            (void)r->hal->pruRead(r->ctx, addrs[i], &b, 1U, &resp);
            Rnd91_w32(r->blk, RND91_OFF_PROBE + (4U * i), ((uint32_t)addrs[i] << 16) | resp);
            Rnd91_w32(r->blk, RND91_OFF_PROBE_N, i + 1U);
        }
    }
    endErr = Rnd91_pruEnd(r);
    return (err != RND91_ERR_NONE) ? err : endErr;
}

static inline uint32_t Rnd91_armP2(Rnd91_Run *r)
{
    uint8_t buf[16] = {0};
    uint16_t resp = 0U;
    uint32_t err = Rnd91_pruBegin(r);
    uint32_t endErr;

    if (err == RND91_ERR_NONE)
    {
        (void)Rnd91_pruMemRead(r, RND91_ADDR_EEPROM, RND91_EEPROM_TEST_OFFSET, buf, sizeof(buf), &resp);
        Rnd91_w32(r->blk, RND91_OFF_P2_RESP, resp);
        Rnd91_putBytes(r->blk, RND91_OFF_P2_DATA, buf, sizeof(buf));
    }
    endErr = Rnd91_pruEnd(r);
    return (err != RND91_ERR_NONE) ? err : endErr;
}

/* P3: for each pattern, the SDK i2c_led_read_write_lld sequence with the PRU as the
 * writer: a PRU phase writes [0x11, pattern], the MCU reads the TPIC2810 shift
 * register back (gated == pattern), a second PRU phase writes [0x22] (load). */
static inline uint32_t Rnd91_armP3(Rnd91_Run *r)
{
    static const uint8_t patterns[RND91_P3_MAX] = {0x55U, 0xAAU, 0x00U};
    uint32_t i;
    uint32_t err = RND91_ERR_NONE;

    Rnd91_w32(r->blk, RND91_OFF_P3_N, 0U);
    for (i = 0U; (i < RND91_P3_MAX) && (err == RND91_ERR_NONE); i++)
    {
        uint32_t e = RND91_OFF_P3 + (RND91_P3_STRIDE * i);
        uint8_t tx[2];
        uint8_t ld[1];
        uint8_t rb = 0U;
        uint16_t resp = 0U;
        uint16_t ldResp = 0U;
        int32_t st;
        uint32_t endErr;

        tx[0] = RND91_TPIC_CMD_SHIFT;
        tx[1] = patterns[i];
        ld[0] = RND91_TPIC_CMD_LOAD;
        err = Rnd91_pruBegin(r);
        if (err == RND91_ERR_NONE)
        {
            (void)r->hal->pruWrite(r->ctx, RND91_ADDR_TPIC2810, tx, 2U, &resp);
        }
        endErr = Rnd91_pruEnd(r);
        if (err == RND91_ERR_NONE)
        {
            err = endErr;
        }
        Rnd91_w32(r->blk, e, patterns[i]);
        Rnd91_w32(r->blk, e + 4U, resp);
        /* No check of err here: the guard alone decides whether the MCU may
         * open, so a PRU that did not stop can never reach the MCU driver. */
        if (Rnd91_mcuBegin(r) != RND91_ERR_NONE)
        {
            break;
        }
        st = r->hal->mcuRead(r->ctx, RND91_ADDR_TPIC2810, &rb, 1U);
        Rnd91_mcuEnd(r);
        Rnd91_w32(r->blk, e + 8U, (uint32_t)st);
        Rnd91_w32(r->blk, e + 12U, rb);
        Rnd91_w32(r->blk, RND91_OFF_P3_N, i + 1U);

        /* second PRU phase: load the outputs (the MCU driver is closed again) */
        err = Rnd91_pruBegin(r);
        if (err == RND91_ERR_NONE)
        {
            (void)r->hal->pruWrite(r->ctx, RND91_ADDR_TPIC2810, ld, 1U, &ldResp);
        }
        endErr = Rnd91_pruEnd(r);
        if (err == RND91_ERR_NONE)
        {
            err = endErr;
        }
        Rnd91_w32(r->blk, RND91_OFF_P3_LOAD + (4U * i), ldResp);
    }
    return err;
}

/* C3: MCU only, one driver open. The control that proves the readback method
 * (SDK sequence, read == 0x5A), then the 0x44 readback recorded and not gated,
 * then the LEDs off. The off sequence is always attempted. */
static inline uint32_t Rnd91_armC3(Rnd91_Run *r)
{
    const Rnd91_Hal *h = r->hal;
    uint8_t shift[2];
    uint8_t load[1];
    uint8_t rb = 0U;
    int32_t st;
    int32_t bad;
    int32_t off;
    uint32_t err = Rnd91_mcuBegin(r);

    if (err != RND91_ERR_NONE)
    {
        return err;
    }
    load[0] = RND91_TPIC_CMD_LOAD;
    shift[0] = RND91_TPIC_CMD_SHIFT;
    shift[1] = RND91_C3_PATTERN_CTRL;
    st = h->mcuWrite(r->ctx, RND91_ADDR_TPIC2810, shift, 2U);
    bad = st;
    Rnd91_w32(r->blk, RND91_OFF_C3_WR1, (uint32_t)st);
    st = h->mcuRead(r->ctx, RND91_ADDR_TPIC2810, &rb, 1U);
    bad |= st;
    Rnd91_w32(r->blk, RND91_OFF_C3_RD1, (uint32_t)st);
    Rnd91_w32(r->blk, RND91_OFF_C3_RD1_VAL, rb);
    st = h->mcuWrite(r->ctx, RND91_ADDR_TPIC2810, load, 1U);
    bad |= st;
    Rnd91_w32(r->blk, RND91_OFF_C3_LD1, (uint32_t)st);
    Rnd91_w32(r->blk, RND91_OFF_C3_N, 4U);

    /* documents the 0x44 readback seen on silicon: nothing here is gated */
    shift[0] = RND91_TPIC_CMD_WR_NOW;
    shift[1] = RND91_C3_PATTERN_DOC;
    rb = 0U;
    st = h->mcuWrite(r->ctx, RND91_ADDR_TPIC2810, shift, 2U);
    Rnd91_w32(r->blk, RND91_OFF_C3_WR2, (uint32_t)st);
    st = h->mcuRead(r->ctx, RND91_ADDR_TPIC2810, &rb, 1U);
    Rnd91_w32(r->blk, RND91_OFF_C3_RD2, (uint32_t)st);
    Rnd91_w32(r->blk, RND91_OFF_C3_RD2_VAL, rb);
    Rnd91_w32(r->blk, RND91_OFF_C3_N, 7U);

    shift[0] = RND91_TPIC_CMD_SHIFT;
    shift[1] = 0x00U;
    off = h->mcuWrite(r->ctx, RND91_ADDR_TPIC2810, shift, 2U);
    st = h->mcuWrite(r->ctx, RND91_ADDR_TPIC2810, load, 1U);
    if (off == 0)
    {
        off = st;
    }
    Rnd91_w32(r->blk, RND91_OFF_C3_OFF, (uint32_t)off);
    Rnd91_w32(r->blk, RND91_OFF_C3_N, 8U);
    Rnd91_mcuEnd(r);
    return ((bad | off) == 0) ? RND91_ERR_NONE : Rnd91_fail(r, RND91_ERR_MCU_XFER);
}

/* P4: save, write, MCU readback, restore, MCU readback. The write is never
 * started before the original is saved, and once it was started the restore
 * is always attempted. */
static inline uint32_t Rnd91_armP4(Rnd91_Run *r)
{
    uint8_t orig[8] = {0};
    uint8_t pat[8];
    uint8_t rb[8] = {0};
    uint8_t wr[2 + 8];
    uint16_t resp = 0U;
    int32_t st;
    uint32_t err;
    uint32_t endErr;
    uint32_t i;
    uint32_t flags = 0U;
    int saved = 0;

    /* phase A: save the original, then write the new bytes */
    err = Rnd91_pruBegin(r);
    if (err == RND91_ERR_NONE)
    {
        st = Rnd91_pruMemRead(r, RND91_ADDR_EEPROM, RND91_EEPROM_WRITE_MIN, orig, 8U, &resp);
        Rnd91_w32(r->blk, RND91_OFF_P4_ORIG_RESP, resp);
        Rnd91_putBytes(r->blk, RND91_OFF_P4_ORIG, orig, 8U);
        saved = ((st == 0) && (resp == COMMAND_SUCCESS));
        if (!saved)
        {
            err = Rnd91_fail(r, RND91_ERR_ORIG_NOT_SAVED);
        }
        else
        {
            for (i = 0U; i < 8U; i++)
            {
                pat[i] = (uint8_t)(orig[i] ^ 0xFFU);   /* always differs from the original */
            }
            Rnd91_putBytes(r->blk, RND91_OFF_P4_NEW, pat, 8U);
            wr[0] = (uint8_t)(RND91_EEPROM_WRITE_MIN >> 8);
            wr[1] = (uint8_t)(RND91_EEPROM_WRITE_MIN & 0xFFU);
            for (i = 0U; i < 8U; i++)
            {
                wr[2U + i] = pat[i];
            }
            flags |= RND91_P4_F_WRITE_ISSUED;
            Rnd91_w32(r->blk, RND91_OFF_P4_FLAGS, flags);
            (void)r->hal->pruWrite(r->ctx, RND91_ADDR_EEPROM, wr, sizeof(wr), &resp);
            Rnd91_w32(r->blk, RND91_OFF_P4_WR_RESP, resp);
            r->hal->sleepUs(r->ctx, RND91_EEPROM_WRITE_CYCLE_US);
            if (resp != COMMAND_SUCCESS)
            {
                err = Rnd91_fail(r, RND91_ERR_PRU_XFER);
            }
        }
    }
    endErr = Rnd91_pruEnd(r);
    if (err == RND91_ERR_NONE)
    {
        err = endErr;
    }

    /* readback 1 (the guard refuses if the PRU did not stop) */
    if (flags & RND91_P4_F_WRITE_ISSUED)
    {
        if (Rnd91_mcuBegin(r) == RND91_ERR_NONE)
        {
            st = r->hal->mcuMemRead(r->ctx, RND91_ADDR_EEPROM, RND91_EEPROM_WRITE_MIN, rb, 8U);
            Rnd91_mcuEnd(r);
            Rnd91_w32(r->blk, RND91_OFF_P4_RB1_STATUS, (uint32_t)st);
            Rnd91_putBytes(r->blk, RND91_OFF_P4_RB1, rb, 8U);
        }
    }

    /* phase B: restore the original, whatever happened to the readback */
    if (flags & RND91_P4_F_WRITE_ISSUED)
    {
        if (Rnd91_pruBegin(r) == RND91_ERR_NONE)
        {
            wr[0] = (uint8_t)(RND91_EEPROM_WRITE_MIN >> 8);
            wr[1] = (uint8_t)(RND91_EEPROM_WRITE_MIN & 0xFFU);
            for (i = 0U; i < 8U; i++)
            {
                wr[2U + i] = orig[i];
            }
            flags |= RND91_P4_F_RESTORE_ISSUED;
            Rnd91_w32(r->blk, RND91_OFF_P4_FLAGS, flags);
            (void)r->hal->pruWrite(r->ctx, RND91_ADDR_EEPROM, wr, sizeof(wr), &resp);
            Rnd91_w32(r->blk, RND91_OFF_P4_RST_RESP, resp);
            r->hal->sleepUs(r->ctx, RND91_EEPROM_WRITE_CYCLE_US);
            if (resp != COMMAND_SUCCESS)
            {
                err = Rnd91_fail(r, RND91_ERR_PRU_XFER);
            }
        }
        endErr = Rnd91_pruEnd(r);
        if (err == RND91_ERR_NONE)
        {
            err = endErr;
        }
        if (Rnd91_mcuBegin(r) == RND91_ERR_NONE)
        {
            st = r->hal->mcuMemRead(r->ctx, RND91_ADDR_EEPROM, RND91_EEPROM_WRITE_MIN, rb, 8U);
            Rnd91_mcuEnd(r);
            Rnd91_w32(r->blk, RND91_OFF_P4_RB2_STATUS, (uint32_t)st);
            Rnd91_putBytes(r->blk, RND91_OFF_P4_RB2, rb, 8U);
        }
    }
    return err;
}

/* Run the arm the session requested. The block was cleared by the session
 * and holds the request; everything else is written here. */
static inline uint32_t Rnd91_runRequested(Rnd91_Run *r, uint32_t iepClkHz)
{
    volatile uint8_t *b = r->blk;
    uint32_t arm = Rnd91_r32(b, RND91_OFF_REQ_ARM);
    uint32_t hz = Rnd91_r32(b, RND91_OFF_REQ_HZ);
    uint32_t err = RND91_ERR_NONE;

    Rnd91_w32(b, RND91_OFF_MAGIC, RND91_MAGIC);
    Rnd91_w32(b, RND91_OFF_VERSION, RND91_VERSION);
    Rnd91_w32(b, RND91_OFF_CANARY, RND91_CANARY);
    Rnd91_w32(b, RND91_OFF_STATE, RND91_STATE_RUNNING);
    Rnd91_event(b, RND91_EV_START, 0U);

    if ((Rnd91_r32(b, RND91_OFF_REQ_MAGIC) != RND91_REQ_MAGIC) || (arm < RND91_ARM_R0) ||
        (arm > RND91_ARM_C3) || (Rnd91_freqSel(hz) == 0U))
    {
        err = Rnd91_fail(r, RND91_ERR_BAD_REQUEST);
    }
    else
    {
        r->busHz = hz;
        Rnd91_w32(b, RND91_OFF_FREQ_SEL, Rnd91_freqSel(hz));
        Rnd91_w32(b, RND91_OFF_TICK_CLKS, Rnd91_clksPerTick(iepClkHz, hz));
        Rnd91_w32(b, RND91_OFF_IEP_INC, Rnd91_iepIncrement(iepClkHz, hz));
        Rnd91_w32(b, RND91_OFF_SCL_TMO_TICKS, Rnd91_sclTimeoutTicks(iepClkHz, hz));
        Rnd91_w32(b, RND91_OFF_SCL_HZ, Rnd91_sclHz(iepClkHz, hz));
        Rnd91_event(b, RND91_EV_ARM_BEGIN, arm);
        switch (arm)
        {
            case RND91_ARM_R0: err = Rnd91_armR0(r); break;
            case RND91_ARM_P1: err = Rnd91_armP1(r); break;
            case RND91_ARM_P2: err = Rnd91_armP2(r); break;
            case RND91_ARM_P3: err = Rnd91_armP3(r); break;
            case RND91_ARM_P4: err = Rnd91_armP4(r); break;
            default:           err = Rnd91_armC3(r); break;
        }
    }
    Rnd91_event(b, RND91_EV_ARM_END, err);
    Rnd91_w32(b, RND91_OFF_STATE, (err == RND91_ERR_NONE) ? RND91_STATE_DONE : RND91_STATE_ABORT);
    return err;
}

#endif /* RND91_BENCH_H_ */
