
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
; JPEG baseline Huffman decoder for PRU0 (pointer-array lookup tables).
;
; Every speed-up is an assembly-time switch (clpru --asm_define=NAME=0/1) so
; each can be measured alone and combined.  A switch that is not defined
; defaults to ON; all switches OFF gives the plain reference decoder.
; Defaults are the shipped configuration.
;
;   O_ZERO   burst zero-fill (6 regs x 24 B) + one 16 B comp-info load
;   O_LONG   canonical per-length limit search for codes > 8 bits
;   O_REFILL left-aligned bit window refilled 32 bits per lbbo (PRU byte-swap)
;   O_EXT    sign extension without per-coefficient mask construction
;   O_R11    count padding (fill) bits only; real bits = r1 - r11
;   O_STORE  AC loop glue: out-of-line long path, rotated loop, tighter store
;   O_FAST   (optional, default OFF) combined code+extension lookup for AC
;            symbols whose code and extra bits fit in 8 bits; needs the host
;            fast table at 0x2000 + 0x400*tid and
;            O_REFILL=1, O_LONG=1, O_STORE=1
;
; Bit window.  Right-aligned (O_REFILL=0): valid bits are the low r1 bits of
; r0, higher bits are stale.  Left-aligned (O_REFILL=1): valid bits are the top
; r1 bits of r0; bits below them are either zero or the *correct* following
; stream bits (refill ORs a whole word at bit r1 but advances the pointer by
; whole bytes only, so the next refill re-ORs identical bits).
    .retain     ; Required for building .out with assembly file
    .retainrefs ; Required for building .out with assembly
    .text
    .global main

    .if $isdefed("O_ZERO")
    .else
O_ZERO .set 1
    .endif
    .if $isdefed("O_LONG")
    .else
O_LONG .set 1
    .endif
    .if $isdefed("O_REFILL")
    .else
O_REFILL .set 1
    .endif
    .if $isdefed("O_EXT")
    .else
O_EXT .set 1
    .endif
    .if $isdefed("O_R11")
    .else
O_R11 .set 1
    .endif
    .if $isdefed("O_STORE")
    .else
O_STORE .set 1
    .endif
    .if $isdefed("O_FAST")
    .else
O_FAST .set 0
    .endif

    .if $isdefed("DUAL_PRU")
    .else
DUAL_PRU .set 0
    .endif
    .if DUAL_PRU
; Dual-PRU build: this core only runs the scan. Each finished block
; is decoded into a staging buffer in PRU0 DRAM and handed to PRU1 through the
; scratchpad banks (ipc_xfr.inc), together with its component index. The
; per-block output-pointer table (CTRL+16) is not used in this build.
DUAL_SAVE  .set 0x1600      ; spill area for R2..R22 live state (84 B)
DUAL_STAGE .set 0x1700      ; 128 B staging block
    .include "ipc_xfr.inc"
    .endif
; Single-core build (SINGLE_CORE=1): this core runs the complete decoder.
; Each finished block is decoded into a fixed 128 B coefficient buffer in
; DRAM (0x1700) and handed to single_core_body.inc, which runs the
; dequantize, inverse DCT, and output stages before the next block is
; scanned. The host supplies the quantization tables at 0x1900 and the
; output pointer at 0x1870; it does no inter-stage hand-off.
    .if $isdefed("SINGLE_CORE")
    .else
SINGLE_CORE .set 0
    .endif

    .if O_FAST
    .if O_REFILL == 0
    .emsg "O_FAST needs O_REFILL=1"
    .endif
    .if O_LONG == 0
    .emsg "O_FAST needs O_LONG=1 (r28/r29 hold the fast-table base)"
    .endif
    .if O_STORE == 0
    .emsg "O_FAST needs O_STORE=1"
    .endif
    .endif

; ---------------------------------------------------------------- macros
; Consume n bits (n is r12/r18/r19), failing 0x12 if that eats padding bits.
; stub restores r1 so the diagnostic matches the pre-consume state.
CONSUME .macro n, stub
    .if O_REFILL
    .if O_R11
    lsl r0, r0, n
    sub r1, r1, n
    qbgt stub, r1, r11
    .else
    qbgt decode_fail, r11, n
    lsl r0, r0, n
    sub r1, r1, n
    sub r11, r11, n
    .endif
    .else
    .if O_R11
    sub r1, r1, n
    qbgt stub, r1, r11
    .else
    qbgt decode_fail, r11, n
    sub r1, r1, n
    sub r11, r11, n
    .endif
    .endif
    .endm

; Right-aligned byte-at-a-time refill until r1 >= n.
ENSURE_R .macro n
    qbgt need?, r1, n
    qba done?
need?:
    qbge fill?, r3, r2
    lbbo &r5, r2, 0, 1
    and r5, r5, 255
    add r2, r2, 1
    lsl r0, r0, 8
    or r0, r0, r5
    add r1, r1, 8
    .if O_R11 == 0
    add r11, r11, 8
    .endif
    qbgt need?, r1, n
    qba done?
fill?:
    ldi r5, 255
    lsl r0, r0, 8
    or r0, r0, r5
    add r1, r1, 8
    .if O_R11
    add r11, r11, 8
    .endif
    qbgt need?, r1, n
    qba done?
done?:
    .endm

; Left-aligned word refill (needs r1 < n <= 16 on entry to the refill).
; Fast path: >= 4 real bytes left in the segment -> one 4-byte lbbo, PRU
; byte-swap to big-endian, OR at bit r1, advance by whole bytes.
; Slow path (segment tail): byte loop with 0xFF lookahead fill, as before.
ENSURE_L .macro n, setop
    qble done?, r1, n
    sub r5, r3, r2
    qbgt slow?, r5, 4
    lbbo &r5, r2, 0, 4
    mov r6.b3, r5.b0
    mov r6.b2, r5.b1
    mov r6.b1, r5.b2
    mov r6.b0, r5.b3
    lsr r6, r6, r1
    or r0, r0, r6
    rsb r7, r1, 32
    lsr r7, r7, 3
    add r2, r2, r7
    lsl r7, r7, 3
    add r1, r1, r7
    .if O_R11 == 0
    add r11, r11, r7
    .endif
    qba done?
slow?:
    setop r6, n
    jal r7, refill_slow
done?:
    .endm

ENSURE .macro n, setop
    .if O_REFILL
    ENSURE_L n, setop
    .else
    ENSURE_R n
    .endif
    .endm

; Baseline linear scan of the long-code partition.
LONGLINEAR .macro sec, done
    lbbo &r13, sec, 0, 1
    and r13, r13, 255
    ldi r5, 4
    add r19, sec, r5
    mov r28, r19
    mov r29, r13
lp?:
    qbeq fail_long, r29, 0
    lbbo &r5, r28, 0, 4
    and r18, r5, 255
    lsr r19, r5, 8
    and r19, r19, 255
    lsr r5, r5, 16
    ldi r6, 16
    sub r6, r6, r18
    lsr r7, r12, r6
    lsr r8, r5, r6
    qbeq fnd?, r7, r8
    add r28, r28, 4
    sub r29, r29, 1
    qba lp?
fnd?:
    CONSUME r18, cstub18
    mov r18, r19
    qba done
    .endm

; Canonical search.  The partition header holds the table offset at +2 (u16);
; the table is 8 x {limit u32, off4 u32} for lengths 9..16 where limit is the
; left-justified exclusive upper bound of the length's codes (0 if none) and
; off4 = 4*(valptr - mincode).  Entry address = sec + 4 + 4*(p>>(16-L)) + off4.
LONGCANON .macro sec, done
    lbbo &r5, sec, 2, 2
    add r5, sec, r5.w0
    lbbo &r6, r5, 0, 4
    qbgt f0?, r12, r6
    lbbo &r6, r5, 8, 4
    qbgt f1?, r12, r6
    lbbo &r6, r5, 16, 4
    qbgt f2?, r12, r6
    lbbo &r6, r5, 24, 4
    qbgt f3?, r12, r6
    lbbo &r6, r5, 32, 4
    qbgt f4?, r12, r6
    lbbo &r6, r5, 40, 4
    qbgt f5?, r12, r6
    lbbo &r6, r5, 48, 4
    qbgt f6?, r12, r6
    lbbo &r6, r5, 56, 4
    qbgt f7?, r12, r6
    qba fail_long
f0?:
    ldi r7, 7
    lbbo &r6, r5, 4, 4
    qba fc?
f1?:
    ldi r7, 6
    lbbo &r6, r5, 12, 4
    qba fc?
f2?:
    ldi r7, 5
    lbbo &r6, r5, 20, 4
    qba fc?
f3?:
    ldi r7, 4
    lbbo &r6, r5, 28, 4
    qba fc?
f4?:
    ldi r7, 3
    lbbo &r6, r5, 36, 4
    qba fc?
f5?:
    ldi r7, 2
    lbbo &r6, r5, 44, 4
    qba fc?
f6?:
    ldi r7, 1
    lbbo &r6, r5, 52, 4
    qba fc?
f7?:
    ldi r7, 0
    lbbo &r6, r5, 60, 4
fc?:
    lsr r8, r12, r7
    lsl r8, r8, 2
    add r8, r8, r6
    add r8, r8, sec
    lbbo &r18, r8, 4, 2
    mov r19, r18.b0
    mov r18, r18.b1
    CONSUME r19, cstub19
    qba done
    .endm

; Long-code path: p16 (16 lookahead bits, right-justified) is in r12.
LONGPATH .macro sec, done
    .if O_LONG
    LONGCANON sec, done
    .else
    LONGLINEAR sec, done
    .endif
    .endm

; p16 -> r12
PEEK16 .macro
    .if O_REFILL
    lsr r12, r0, 16
    .else
    ENSURE_R 16
    sub r12, r1, 16
    lsr r12, r0, r12
    ldi r6, 0xFFFF
    and r12, r12, r6
    .endif
    .endm

; Decode one Huffman symbol into r18 and fall into/branch to done.
; lut = 256 x {len<<4, sym} primary table base, sec = long-code partition.
SYMDECODE .macro lut, sec, lng, done, ens
    .if O_REFILL
    .if ens == 1
    ENSURE_L 16, ldi
    lsr r12, r0, 24
    .endif
    lsl r12, r12, 1
    lbbo &r18, lut, r12, 2
    lsr r12, r18.b0, 4
    qbeq lng, r12, 0
    CONSUME r12, cstub12
    mov r18, r18.b1
    .else
    ENSURE_R 8
    sub r12, r1, 8
    lsr r12, r0, r12
    and r12, r12, 255
    lsl r13, r12, 1
    add r13, lut, r13
    lbbo &r18, r13, 0, 2
    and r12, r18, 255
    lsr r12, r12, 4
    and r12, r12, 15
    qbeq lng, r12, 0
    CONSUME r12, cstub12
    lsr r18, r18, 8
    and r18, r18, 255
    .endif
    .if O_STORE == 0
    qba done
    .endif
    .endm

; Extension bits: r12 = size (1..15) -> r8 = sign-extended value, bits consumed.
EXTVAL .macro
    ENSURE r12, mov
    .if O_REFILL
    .if O_EXT
    rsb r7, r12, 32
    qbbs pos?, r0, 31
    not r8, r0
    lsr r8, r8, r7
    rsb r8, r8, 0
    qba cons?
pos?:
    lsr r8, r0, r7
cons?:
    CONSUME r12, cstub12
    .else
    rsb r6, r12, 32
    lsr r8, r0, r6
    CONSUME r12, cstub12
    sub r6, r12, 1
    ldi r7, 1
    lsl r7, r7, r6
    qbge noext?, r7, r8
    ldi r6, 1
    lsl r6, r6, r12
    sub r6, r6, 1
    sub r8, r8, r6
noext?:
    .endif
    .else
    .if O_EXT
    sub r6, r1, r12
    lsr r8, r0, r6
    ldi r7, 1
    lsl r7, r7, r12
    sub r6, r7, 1
    and r8, r8, r6
    lsr r7, r7, 1
    CONSUME r12, cstub12
    qble noext?, r8, r7
    sub r8, r8, r6
noext?:
    .else
    sub r6, r1, r12
    lsr r8, r0, r6
    ldi r6, 1
    lsl r6, r6, r12
    sub r6, r6, 1
    and r8, r8, r6
    CONSUME r12, cstub12
    sub r6, r12, 1
    ldi r7, 1
    lsl r7, r7, r6
    qbge noext?, r7, r8
    ldi r6, 1
    lsl r6, r6, r12
    sub r6, r6, 1
    sub r8, r8, r6
noext?:
    .endif
    .endif
    .endm

; ---------------------------------------------------------------- main
main:
    ldi r0, 0
    ldi r1, 0
    ; r11: O_R11=0 counts real (not lookahead-fill) bits in r0;
    ;      O_R11=1 counts the lookahead-fill bits (real bits = r1 - r11).
    ldi r11, 0
    ldi32 r10, 0x00001000
    ; Segment bounds come from the host segment table (CTRL+40), entry 0.
    ; The restart path pre-increments the index cell, so entry 0 is read
    ; here and only here; restarts start at entry 1.
    lbbo &r6, r10, 40, 4
    ldi32 r12, 0x00001030
    ldi r13, 0
    sbbo &r13, r12, 0, 4
    lbbo &r2, r6, 0, 4
    lbbo &r3, r6, 4, 4
    lbbo &r5, r10, 12, 4
    lbbo &r20, r10, 16, 4
    lbbo &r21, r10, 20, 4
    lbbo &r22, r10, 24, 4
    lbbo &r9, r10, 28, 4
    lbbo &r14, r10, 32, 4
    lbbo &r10, r10, 36, 4
    mov r15, r5
    ldi r5, 0
    sbbo &r5, r14, 0, 4
    sbbo &r5, r14, 4, 4
    sbbo &r5, r14, 8, 4
    ldi r4, 0
scan_block_loop:
    qbge block_done, r15, r4
    .if DUAL_PRU
    ldi r16, DUAL_STAGE
    .elseif SINGLE_CORE
    ldi r16, 0x1700
    .else
    lsl r12, r4, 2
    add r12, r20, r12
    lbbo &r16, r12, 0, 4
    .endif
    add r12, r21, r4
    lbbo &r17, r12, 0, 1
    and r17, r17, 255
    add r12, r22, r4
    lbbo &r18, r12, 0, 1
    and r18, r18, 255
    qbeq no_restart, r18, 0
    ldi r5, 0
    sbbo &r5, r14, 0, 4
    sbbo &r5, r14, 4, 4
    sbbo &r5, r14, 8, 4
    ldi r0, 0
    ldi r1, 0
    ldi r11, 0
    ; Segment table pointer is CTRL+40; its mutable index lives in DRAM
    ; because R30/R31 are PRU I/O registers, not general persistent state.
    ldi32 r6, 0x00001030
    lbbo &r12, r6, 0, 4
    add r12, r12, 1
    sbbo &r12, r6, 0, 4
    lsl r12, r12, 3
    ldi32 r6, 0x00001028
    lbbo &r6, r6, 0, 4
    add r12, r6, r12
    lbbo &r2, r12, 0, 4
    lbbo &r3, r12, 4, 4
no_restart:
    lsl r12, r17, 4
    add r12, r9, r12
    .if O_ZERO
    ; Zero the 128 B coefficient block from a zeroed register bank, then
    ; fetch the four table bases (dc lut, ac lut, dc sec, ac sec) in one lbbo.
    zero &r24, 24
    sbbo &r24, r16, 0, 24
    sbbo &r24, r16, 24, 24
    sbbo &r24, r16, 48, 24
    sbbo &r24, r16, 72, 24
    sbbo &r24, r16, 96, 24
    sbbo &r24, r16, 120, 8
    lbbo &r24, r12, 0, 16
    .if O_FAST
    ; fast table base = 2 * ac_lut_base + 0x1780  (0x2000 + 0x400*tid)
    lsl r28, r25, 1
    ldi r29, 0x1780
    add r28, r28, r29
    .endif
    .else
    lbbo &r19, r12, 0, 4
    mov r24, r19
    lbbo &r19, r12, 4, 4
    mov r25, r19
    lbbo &r19, r12, 8, 4
    mov r26, r19
    lbbo &r19, r12, 12, 4
    mov r27, r19
    ldi r19, 0
    ldi r5, 0
zero_loop:
    qble zero_done, r5, 128
    sbbo &r19, r16, r5, 4
    add r5, r5, 4
    qba zero_loop
zero_done:
    .endif
    ; DC symbol
    SYMDECODE r24, r26, dc_long, dc_sym_done, 1
    .if O_STORE == 0
dc_long:
    PEEK16
    LONGPATH r26, dc_sym_done
    .endif
dc_sym_done:
    qbeq dc_store_noext, r18, 0
    mov r12, r18
    EXTVAL
    lsl r13, r17, 2
    add r13, r14, r13
    lbbo &r6, r13, 0, 4
    add r6, r6, r8
    sbbo &r6, r13, 0, 4
    mov r8, r6
    qba dc_store_val
dc_store_noext:
    lsl r13, r17, 2
    add r13, r14, r13
    lbbo &r8, r13, 0, 4
dc_store_val:
    sbbo &r8, r16, 0, 2
    ldi r23, 1
    .if O_STORE
    ; Rotated loop: the k<64 test sits at the bottom, so a continuing
    ; coefficient costs one branch instead of two.
ac_loop:
    .if O_FAST
    ; Combined lookup: 4 B entries {code+ext bits, run, value16}; b0 == 0
    ; means "not fast", fall into the ordinary decode below.
    ENSURE_L 16, ldi
    lsr r12, r0, 24
    lsl r13, r12, 2
    lbbo &r13, r28, r13, 4
    qbeq fmiss, r13.b0, 0
    CONSUME r13.b0, cstubfa
    add r23, r23, r13.b1
    qble ac_done, r23, 64
    lsr r8, r13, 16
    lbbo &r7.b0, r10, r23, 1
    lsl r7, r7.b0, 1
    sbbo &r8, r16, r7, 2
    add r23, r23, 1
    qbgt ac_loop, r23, 64
    qba ac_done
fmiss:
    SYMDECODE r25, r27, ac_long, ac_sym_done, 2
    .else
    SYMDECODE r25, r27, ac_long, ac_sym_done, 1
    .endif
ac_sym_done:
    lsr r12, r18, 4
    and r13, r18, 15
    qbeq ac_eob, r13, 0
    add r23, r23, r12
    qble ac_done, r23, 64
    mov r12, r13
    EXTVAL
    lbbo &r7.b0, r10, r23, 1
    lsl r7, r7.b0, 1
    sbbo &r8, r16, r7, 2
    add r23, r23, 1
    qbgt ac_loop, r23, 64
    qba ac_done
ac_eob:
    qbeq ac_done, r12, 0
    qbne ac_done, r12, 15
    add r23, r23, 16
    qbgt ac_loop, r23, 64
    qba ac_done
    .else
ac_loop:
    qble ac_done, r23, 64
    SYMDECODE r25, r27, ac_long, ac_sym_done, 1
ac_long:
    PEEK16
    LONGPATH r27, ac_sym_done
ac_sym_done:
    lsr r12, r18, 4
    and r12, r12, 15
    and r13, r18, 15
    qbeq ac_eob, r13, 0
    add r23, r23, r12
    qble ac_done, r23, 64
    mov r12, r13
    EXTVAL
    add r6, r10, r23
    lbbo &r7, r6, 0, 1
    and r7, r7, 255
    lsl r7, r7, 1
    add r7, r16, r7
    sbbo &r8, r7, 0, 2
    add r23, r23, 1
    qba ac_loop
ac_eob:
    qbeq ac_is_eob, r12, 0
    qbne ac_done, r12, 15
    add r23, r23, 16
    qba ac_loop
ac_is_eob:
    qba ac_done
    .endif
ac_done:
    .if DUAL_PRU
    ; Send the block. IPC_TX stages through R2..R23, which hold the decoder's live
    ; state (r2 r3 segment, r4 block index, r9 r10 r11, r14 r15, r20..r22), so
    ; spill those to DRAM around it. r16..r18 are dead here (reloaded at the
    ; top of the loop); r26..r29 are reloaded per block too.
    add r26, r4, 1              ; sequence number = block index + 1
    ldi r27, DUAL_STAGE
    mov r28, r17                ; component index -> PRU1 qtable selector
    ldi r29, DUAL_SAVE
    sbbo &r2, r29, 0, 12
    sbbo &r9, r29, 12, 12
    sbbo &r14, r29, 24, 8
    sbbo &r20, r29, 32, 12
    IPC_TX r26, r27, r28
    lbbo &r2, r29, 0, 12
    lbbo &r9, r29, 12, 12
    lbbo &r14, r29, 24, 8
    lbbo &r20, r29, 32, 12
    .elseif SINGLE_CORE
    jmp single_stage
    .endif
single_return:
    add r4, r4, 1
    qba scan_block_loop
    .if O_STORE
    ; Long-code paths live out of line so the short path never jumps over them.
dc_long:
    PEEK16
    LONGPATH r26, dc_sym_done
ac_long:
    PEEK16
    LONGPATH r27, ac_sym_done
    .endif
block_done:
    ; Completion flag lets the host distinguish normal end from truncated input.
    ldi32 r5, 0x00001008
    ldi r6, 1
    sbbo &r6, r5, 0, 4
    halt
decode_fail:
    ; 0x12 means a symbol/extension required lookahead-fill bits.  Keep
    ; completed clear and expose the count for host truncation diagnostics.
    ; Diagnostics live at 0x1100/0x1104, outside the 11-word control
    ; struct at 0x1000 (whose word at +0x27 they used to clobber).
    .if O_R11
    sub r11, r1, r11
    .endif
    ldi32 r5, 0x00001100
    ldi r6, 0x12
    sbbo &r6, r5, 0, 1
    add r5, r5, 4
    sbbo &r11, r5, 0, 4
    halt
fail_long:
    qba invalid_code
invalid_code:
    ; No Huffman code matched the lookahead.  If the bit buffer holds
    ; lookahead-fill bits the mismatch was caused by scan truncation, not a
    ; corrupt table: report 0x12 like the guards.  Only an invalid code in an
    ; all-real buffer is 0x13.
    ldi32 r5, 0x00001100
    .if O_R11
    qbne invalid_code_trunc, r11, 0
    sub r11, r1, r11
    .else
    qbgt invalid_code_trunc, r11, r1
    .endif
    ldi r6, 0x13
    qba invalid_code_write
invalid_code_trunc:
    .if O_R11
    sub r11, r1, r11
    .endif
    ldi r6, 0x12
invalid_code_write:
    sbbo &r6, r5, 0, 1
    add r5, r5, 4
    sbbo &r11, r5, 0, 4
    halt
    .if O_R11
    ; Guard-failure stubs: undo the consume so the diagnostic is pre-consume.
cstub12:
    add r1, r1, r12
    qba decode_fail
cstub18:
    add r1, r1, r18
    qba decode_fail
cstub19:
    add r1, r1, r19
    qba decode_fail
    .if O_FAST
cstubfa:
    add r1, r1, r13.b0
    qba decode_fail
    .endif
    .endif
    .if O_REFILL
; Left-aligned byte refill for the segment tail.  In: r6 = bits needed,
; link r7.  Clobbers r5, r19.
refill_slow:
sl_loop:
    qble sl_done, r1, r6
    qbge sl_fill, r3, r2
    lbbo &r5, r2, 0, 1
    and r5, r5, 255
    add r2, r2, 1
    rsb r19, r1, 24
    lsl r5, r5, r19
    or r0, r0, r5
    add r1, r1, 8
    .if O_R11 == 0
    add r11, r11, 8
    .endif
    qba sl_loop
sl_fill:
    ldi r5, 255
    rsb r19, r1, 24
    lsl r5, r5, r19
    or r0, r0, r5
    add r1, r1, 8
    .if O_R11
    add r11, r11, 8
    .endif
    qba sl_loop
sl_done:
    jmp r7
    .endif
    .if SINGLE_CORE
    .include "single_core_body.inc"
    .endif
