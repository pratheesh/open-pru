/* File: main.c */
/*
 *  Copyright (C) 2026 Texas Instruments Incorporated
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  LP-AM263 PRU I2C master bench (RND-91), R5F NORTOS side. Not run on silicon.
 *
 *  The PRU1 I2C master firmware drives the bus on PR0_PRU1_GPIO7 (SCL, ball
 *  A16, J8-74) and PR0_PRU1_GPIO17 (SDA, ball D13, J8-73). Two wires take
 *  those to J5-49 / J5-50, the I2C1 bus of the board (EEPROM 0x52/0x53,
 *  TPIC2810 0x61). The MCU I2C1 controller (pads D7/C8) is on the same wires,
 *  so it is opened only while the PRU is stopped with both lines released:
 *  the order is enforced by Rnd91_Guard in rnd91_bench.h.
 *
 *  One run executes one arm, chosen by the session through the request words
 *  in the result block (see rnd91_bench.h and examples/pru_i2c/am263x-lp-bench.md).
 *
 *  Drivers_open() is not used: it would open the I2C1 driver before the PRU
 *  has run. The pieces that are needed are opened by hand.
 */

#include <stdint.h>
#include <string.h>
#include <kernel/dpl/DebugP.h>
#include <kernel/dpl/ClockP.h>
#include "ti_drivers_config.h"
#include "ti_drivers_open_close.h"
#include "ti_board_config.h"
#include <drivers/pruicss.h>
#include <drivers/i2c.h>
#include <drivers/soc.h>
#include <drivers/hw_include/hw_types.h>
#include <drivers/hw_include/cslr_soc.h>
#include <pru1_load_bin.h>

#include "rnd91_bench.h"

/* How long the R5F waits for the PRU to report a response */
#define RND91_PRU_TIMEOUT_US        (100000U)
/* MCU I2C1 transfers: the SDK i2c_memory_read geometry */
#define RND91_EEPROM_ADDR_SIZE      (2U)
#define RND91_OUT_CTRL_REG          (CSL_MSS_CTRL_U_BASE + CSL_MSS_CTRL_ICSSM_PRU1_GPIO_OUT_CTRL)

static PRUICSS_Handle gPruIcss0Handle;
static volatile uint8_t *gDmem0;    /* PRU0 data RAM: the global words */
static volatile uint8_t *gDmem1;    /* PRU1 data RAM: the instance block */
static volatile uint8_t *gBlock = (volatile uint8_t *)RND91_BLOCK_ADDR;

/* ========================================================================== */
/*                        PRU side (Rnd91_Hal functions)                      */
/* ========================================================================== */

static int32_t App_pruSend(uint16_t command, uint16_t *resp)
{
    uint64_t start;

    __sync_synchronize();
    HW_WR_REG16(gDmem1 + ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_COMMAND_OFFSET + 2U, command);
    start = ClockP_getTimeUsec();
    while (!Rnd91_irqPending(gDmem0))
    {
        if ((ClockP_getTimeUsec() - start) > RND91_PRU_TIMEOUT_US)
        {
            /* no response from the firmware (a firmware SCL timeout is a TIME_OUT_ERROR response) */
            *resp = 0xFFFFU;
            return SystemP_TIMEOUT;
        }
    }
    *resp = HW_RD_REG16(gDmem1 + ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_COMMAND_OFFSET);
    Rnd91_irqAck(gDmem0);
    return (*resp == COMMAND_SUCCESS) ? SystemP_SUCCESS : SystemP_FAILURE;
}

static int32_t App_pruStart(void *ctx, uint32_t busHz)
{
    uint16_t resp = 0U;
    uint32_t reg;
    int32_t status;

    (void)ctx;
    /* PRU1 off, IEP counter off (the firmware starts it again and programs
     * the compare from the new increment) */
    status = PRUICSS_disableCore(gPruIcss0Handle, RND91_PRU_CORE);
    (void)PRUICSS_controlIepCounter(gPruIcss0Handle, 0U, 0U);
    if ((status != SystemP_SUCCESS) || (PRUICSS_initMemory(gPruIcss0Handle, PRUICSS_DATARAM(RND91_PRU_CORE)) == 0U))
    {
        return SystemP_FAILURE;
    }

    /* PRU1 GPIO = direct GPIO mode, GPI taken from the pad (not PWMXBAR) */
    (void)PRUICSS_setGpMuxSelect(gPruIcss0Handle, RND91_PRU_CORE, PRUICSS_GP_MUX_SEL_MODE_GP);
    (void)PRUICSS_setGpiMode(gPruIcss0Handle, RND91_PRU_CORE, PRUICSS_GPI_MODE_DIRECT_R31_STATUS);
    SOC_selectIcssGpiMux(RND91_PRU_CORE, 0x0U);

    Rnd91_fillPruConfig(gDmem0, gDmem1, Rnd91_freqSel(busHz),
                        Rnd91_iepIncrement(CONFIG_PRU_ICSS0_IEP_CLK_FREQ_HZ, busHz),
                        Rnd91_sclTimeoutTicks(CONFIG_PRU_ICSS0_IEP_CLK_FREQ_HZ, busHz));

    /* The firmware writes MSS_CTRL ICSSM_PRU1_GPIO_OUT_CTRL itself, and that
     * partition is MMR-locked after System_init. Unlock it for as long as the
     * PRU runs (App_pruStopRelease locks it again). Start with both lines
     * released: OUT_CTRL bit = 1 disables the output driver. */
    SOC_controlModuleUnlockMMR(SOC_DOMAIN_ID_MAIN, MSS_CTRL_PARTITION0);
    reg = HW_RD_REG32(RND91_OUT_CTRL_REG);
    HW_WR_REG32(RND91_OUT_CTRL_REG, reg | RND91_PIN_MASK);

    status = PRUICSS_loadFirmware(gPruIcss0Handle, RND91_PRU_CORE, PRU1Firmware_0, sizeof(PRU1Firmware_0));
    if (status != SystemP_SUCCESS)
    {
        return status;
    }
    status = App_pruSend(ICSS_I2C_SETUP_CMD, &resp);
    DebugP_log("PRU setup: response 0x%04X\r\n", resp);
    return status;
}

static int32_t App_pruStopRelease(void *ctx, uint32_t *ctrlReg, uint32_t *outCtrl)
{
    uintptr_t ctrlAddr = PRUICSS_getAttrs(CONFIG_PRU_ICSS0)->pru1CtrlRegBase + CSL_ICSS_M_PR1_PDSP1_IRAM_CONTROL;
    int32_t status;

    (void)ctx;
    status = PRUICSS_disableCore(gPruIcss0Handle, RND91_PRU_CORE);
    /* release both lines whatever R30 holds, then read back what is there */
    HW_WR_REG32(RND91_OUT_CTRL_REG, HW_RD_REG32(RND91_OUT_CTRL_REG) | RND91_PIN_MASK);
    __sync_synchronize();
    *ctrlReg = HW_RD_REG32(ctrlAddr);
    *outCtrl = HW_RD_REG32(RND91_OUT_CTRL_REG);
    SOC_controlModuleLockMMR(SOC_DOMAIN_ID_MAIN, MSS_CTRL_PARTITION0);
    return status;
}

static int32_t App_pruWrite(void *ctx, uint8_t addr7, const uint8_t *data, uint32_t len, uint16_t *resp)
{
    uint32_t i;

    (void)ctx;
    HW_WR_REG16(gDmem1 + ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_SA_OFFSET, addr7);
    HW_WR_REG16(gDmem1 + ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_CNT_OFFSET, (uint16_t)len);
    for (i = 0U; i < len; i++)
    {
        HW_WR_REG8(gDmem1 + ICSS_I2C_INSTANCE0_TX_MEM + i, data[i]);
    }
    return App_pruSend(ICSS_I2C_TX_CMD, resp);
}

static int32_t App_pruRead(void *ctx, uint8_t addr7, uint8_t *data, uint32_t len, uint16_t *resp)
{
    uint32_t i;
    int32_t status;

    (void)ctx;
    HW_WR_REG16(gDmem1 + ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_SA_OFFSET, addr7);
    HW_WR_REG16(gDmem1 + ICSS_I2C_INSTANCE0_ADDR + ICSS_I2C_CNT_OFFSET, (uint16_t)len);
    status = App_pruSend(ICSS_I2C_RX_CMD, resp);
    if (status == SystemP_SUCCESS)
    {
        for (i = 0U; i < len; i++)
        {
            data[i] = HW_RD_REG8(gDmem1 + ICSS_I2C_INSTANCE0_RX_MEM + i);
        }
    }
    return status;
}

/* ========================================================================== */
/*                  MCU I2C1 side (Rnd91_Hal functions)                       */
/* ========================================================================== */

static int32_t App_mcuOpen(void *ctx)
{
    (void)ctx;
    Drivers_i2cOpen();
    return (gI2cHandle[CONFIG_I2C0] != NULL) ? SystemP_SUCCESS : SystemP_FAILURE;
}

static void App_mcuClose(void *ctx)
{
    (void)ctx;
    Drivers_i2cClose();
}

static int32_t App_mcuRead(void *ctx, uint8_t addr7, uint8_t *data, uint32_t len)
{
    I2C_Transaction txn;

    (void)ctx;
    I2C_Transaction_init(&txn);
    txn.readBuf = data;
    txn.readCount = len;
    txn.targetAddress = addr7;
    return (I2C_transfer(gI2cHandle[CONFIG_I2C0], &txn) == SystemP_SUCCESS) ? 0 : -1;
}

static int32_t App_mcuWrite(void *ctx, uint8_t addr7, const uint8_t *data, uint32_t len)
{
    I2C_Transaction txn;

    (void)ctx;
    I2C_Transaction_init(&txn);
    txn.writeBuf = (void *)data;
    txn.writeCount = len;
    txn.targetAddress = addr7;
    return (I2C_transfer(gI2cHandle[CONFIG_I2C0], &txn) == SystemP_SUCCESS) ? 0 : -1;
}

/* The SDK i2c_memory_read geometry: 2-byte memory address */
static int32_t App_mcuMemRead(void *ctx, uint8_t addr7, uint16_t memAddr, uint8_t *data, uint32_t len)
{
    I2C_Transaction     txn;
    I2C_Mem_Transaction mem;

    (void)ctx;
    I2C_Transaction_init(&txn);
    txn.memTxnEnable = true;
    txn.memTransaction = &mem;
    txn.targetAddress = addr7;
    mem.memAddr = memAddr;
    mem.memAddrSize = RND91_EEPROM_ADDR_SIZE;
    mem.buffer = data;
    mem.size = len;
    mem.memDataDir = I2C_MEM_TXN_DIR_RX;
    return (I2C_transfer(gI2cHandle[CONFIG_I2C0], &txn) == SystemP_SUCCESS) ? 0 : -1;
}

static void App_sleepUs(void *ctx, uint32_t us)
{
    (void)ctx;
    ClockP_usleep(us);
}

static const Rnd91_Hal gHal = {
    App_pruStart, App_pruStopRelease, App_pruWrite, App_pruRead,
    App_mcuOpen, App_mcuClose, App_mcuRead, App_mcuWrite, App_mcuMemRead, App_sleepUs,
};

/* ========================================================================== */
/*                                   main                                     */
/* ========================================================================== */

int main(void)
{
    Rnd91_Run run;
    const PRUICSS_HwAttrs *hwAttrs;
    uint32_t err;

    System_init();
    Board_init();
    /* the log UART only; the I2C1 driver stays closed until the guard allows it */
    Drivers_uartOpen();

    gPruIcss0Handle = PRUICSS_open(CONFIG_PRU_ICSS0);
    DebugP_assert(gPruIcss0Handle != NULL);
    hwAttrs = PRUICSS_getAttrs(CONFIG_PRU_ICSS0);
    DebugP_assert(hwAttrs != NULL);
    gDmem0 = (volatile uint8_t *)hwAttrs->pru0DramBase;
    gDmem1 = (volatile uint8_t *)hwAttrs->pru1DramBase;

    /* PRU1 must be off, with the lines released, before anything else */
    (void)PRUICSS_disableCore(gPruIcss0Handle, RND91_PRU_CORE);

    run.hal = &gHal;
    run.ctx = NULL;
    run.blk = gBlock;
    run.guard.blk = gBlock;
    run.guard.owner = RND91_OWNER_NONE;
    run.busHz = 0U;

    err = Rnd91_runRequested(&run, CONFIG_PRU_ICSS0_IEP_CLK_FREQ_HZ);
    DebugP_log("RND91 arm %u, error %u, state %u\r\n", Rnd91_r32(gBlock, RND91_OFF_REQ_ARM),
               err, Rnd91_r32(gBlock, RND91_OFF_STATE));
    DebugP_log(err == RND91_ERR_NONE ? "RND91 arm complete\r\n" : "RND91 arm aborted\r\n");

    /* leave PRU1 off and the log flushed; the session reads the block */
    (void)PRUICSS_disableCore(gPruIcss0Handle, RND91_PRU_CORE);
    for (;;)
    {
    }
}
