
; Copyright (C) 2024-2025 Texas Instruments Incorporated - http://www.ti.com/
;
; Redistribution and use in source and binary forms, with or without
; modification, are permitted provided that the following conditions
; are met:
;
;        * Redistributions of source code must retain the above copyright
;          notice, this list of conditions and the following disclaimer.
;
;        * Redistributions in binary form must reproduce the above copyright
;          notice, this list of conditions and the following disclaimer in the
;          documentation and/or other materials provided with the
;          distribution.
;
;        * Neither the name of Texas Instruments Incorporated nor the names of
;          its contributors may be used to endorse or promote products derived
;          from this software without specific prior written permission.
;
; THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
; "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
; LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
; A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
; OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
; SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
; LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
; DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
; THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
; (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
; OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; The Huffman decoder's R0..R23 contain live scan state. The IDCT clobbers nearly all of it,
; so spill the complete range before running the three shared stage bodies.
S_COEF .set 0x1700
S_DEQ .set 0x1100
S_WS .set 0x1500
S_IDCT_OUT .set 0x1600
S_SAVE .set 0x1800
S_OUTPTR .set 0x1870
IDCT_IN .set S_DEQ
IDCT_WS .set S_WS
IDCT_OUT .set S_IDCT_OUT
single_stage:
    ldi r29, S_SAVE
    sbbo &r0, r29, 0, 96
    ; The host places natural-order component qtables at 0x1900.
    lsl r10, r17, 7
    ldi r12, 0x1900
    add r10, r10, r12
    ldi r11, S_COEF
    ldi r12, S_DEQ
    .include "dequant_mac_body.inc"
    .include "idct_body.inc"
    ldi r0, S_OUTPTR
    lbbo &r12, r0, 0, 4
    add r13, r12, 64
    sbbo &r13, r0, 0, 4
    ldi r11, S_IDCT_OUT
    .include "output_stage_body.inc"
    ldi r29, S_SAVE
    lbbo &r0, r29, 0, 96
    jmp single_return
