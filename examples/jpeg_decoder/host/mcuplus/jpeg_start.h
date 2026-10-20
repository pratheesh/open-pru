/*
 *  Copyright (C) 2024-2025 Texas Instruments Incorporated
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
#ifndef JPEG_START_H
#define JPEG_START_H
#include <stdint.h>

/*
 * Start stubs, loaded at IRAM word 0x2F00/4 of each PRU and entered through
 * the CONTROL register PCTR_RST_VAL field. They enable both PRU CYCLE
 * counters (PRU0) and clear the scratchpad banks used by the block hand-off
 * protocol (PRU1), then jump to the decoder entry point at address 0.
 * The host also sets COUNTER_ENABLE in its CONTROL write (jpeg_host.c): on
 * ICSSM the stub's read-modify-write can lose a race with the host driver's
 * own read-modify-write in PRUICSS_enableCore(), so the stub alone is not
 * enough there. The stub is kept so the counters run under any host.
 *
 * gStartPru0 is the PRU CGT 2.3.3 output of:
 *     LDI32 r0, 0x24000        ; PRU1 CONTROL
 *     LBBO  &r1, r0, 0, 4
 *     SET   r1, r1, 3          ; COUNTER_ENABLE
 *     SBBO  &r1, r0, 0, 4
 *     LDI32 r0, 0x22000        ; PRU0 CONTROL
 *     LBBO  &r1, r0, 0, 4
 *     SET   r1, r1, 3
 *     SBBO  &r1, r0, 0, 4
 *     ZERO  &r0, 120
 *     JMP   0
 *
 * gStartPru1 is the PRU CGT 2.3.3 output of:
 *     ZERO  &r0, 120
 *     XOUT  10, &r0, 120       ; scratchpad bank 0
 *     XOUT  11, &r0, 120       ; scratchpad bank 1
 *     XOUT  12, &r0, 120       ; scratchpad bank 2
 *     JMP   0
 */
static const uint32_t gStartPru0[] = {
    0x240002c0U, 0x24400080U, 0xf1002081U, 0x1f03e1e1U, 0xe1002081U, 0x240002c0U,
    0x24200080U, 0xf1002081U, 0x1f03e1e1U, 0xe1002081U, 0x2effbb80U, 0x21000000U,
};

static const uint32_t gStartPru1[] = {
    0x2effbb80U, 0x2f053b80U, 0x2f05bb80U, 0x2f063b80U, 0x21000000U,
};

#endif
