// Host tests for the RND-91 bench. No board, no probe.
//   node bench/test_rnd91.mjs
// 1. compiles the REAL rnd91_bench.h against a fake Rnd91_Hal with gcc in WSL (bench/test_rnd91_host.c) and runs it
//    (timing arithmetic, PRU config image, bus guard, arm sequencer);
// 2. decodes the blocks that C run produced with the REAL JS decoder and judges them with the REAL gates;
// 3. checks the gates on blocks that were patched into failures, the plan runner and the session's launch gate.
// bench/test_rnd91_mutants.mjs runs this file against mutated copies and requires every mutant to be killed.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { pathToFileURL, fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const lib = await import(pathToFileURL(path.join(HERE, "rnd91_lib.mjs")).href);
const session = await import(pathToFileURL(path.join(HERE, "rnd91_session.mjs")).href);
const { decodeBlock, judgeArm, judgeCommon, checkOrdering, armPlan, runPlan, loadLayout, HEADER, IFACE } = lib;

let failed = 0, ran = 0;
const check = (cond, name, extra = "") => { ran++; if (cond) console.log(`ok   ${name}`); else { failed++; console.log(`FAIL ${name} ${extra}`); } };
const wsl = (p) => p.replace(/\\/g, "/").replace(/^([A-Za-z]):/, (m, d) => `/mnt/${d.toLowerCase()}`);

// ---- 1. the C host test ----------------------------------------------------------------------------
const outDir = fs.mkdtempSync(path.join(os.tmpdir(), "rnd91-host-"));
const cSrc = path.join(HERE, "test_rnd91_host.c");
const sh = [
    `set -e`, `gcc -std=c99 -Wall -Wextra -Werror -I${wsl(path.dirname(HEADER))} -I${wsl(path.dirname(IFACE))} -o /tmp/rnd91_host_$$ ${wsl(cSrc)}`,
    `/tmp/rnd91_host_$$ ${wsl(outDir)}; rc=$?; rm -f /tmp/rnd91_host_$$; exit $rc`,
].join("\n");
const c = spawnSync("wsl", ["-d", "Ubuntu-24.04", "--", "bash", "-c", sh], { encoding: "utf8", timeout: 120000 });
const cOut = (c.stdout || "") + (c.stderr || "");
for (const l of cOut.split(/\r?\n/)) if (/^(ok|FAIL) /.test(l)) { ran++; if (l.startsWith("FAIL")) failed++; console.log(`C    ${l}`); }
check(c.status === 0, "C host test: exit 0 (real rnd91_bench.h against the fake HAL)", c.status !== 0 ? cOut.slice(-600) : "");
check(/\d+ checks, 0 failed/.test(cOut), "C host test: all its checks ran and none failed");

const L = loadLayout();
const dump = (name) => fs.readFileSync(path.join(outDir, `${name}.bin`));
const have = fs.existsSync(path.join(outDir, "nominal-1-100.bin"));
check(have, "C host test produced the nominal block dumps");

if (have) {
    // ---- 2. real decoder + real gates on blocks the real C produced -----------------------------------
    const cookieOf = (arm) => 0x52000000 + arm;
    const r0 = decodeBlock(dump("nominal-1-100"), L);
    check(r0.valid && r0.state === L.RND91_STATE_DONE && r0.err === 0, "decoder: R0 block valid, DONE, no error");
    check(r0.r0.data.length === 16 && r0.r0.data[0] === ((0xA0 + 0x400 * 7) & 0xff), "decoder: R0 data = the fake EEPROM at 0x0400");
    const r0Data = r0.r0.data;
    for (const [hz, k] of [[100000, 100], [400000, 400]]) {
        for (const arm of [1, 2, 3, 4, 5]) {
            const d = decodeBlock(dump(`nominal-${arm}-${k}`), L);
            const j = judgeArm(arm, d, { arm, hz, cookie: cookieOf(arm), r0Data }, L);
            check(j.ok, `gate: nominal arm ${arm} @${k}k passes`, j.reasons.join("; "));
        }
    }
    // C3 (MCU only, 100 kHz): control gated, the 0x44 readback recorded only
    {
        const d = decodeBlock(dump("nominal-6-100"), L);
        const j = judgeArm(6, d, { arm: 6, hz: 100000, cookie: cookieOf(6), r0Data }, L);
        check(j.ok, "gate: nominal C3 @100k passes", j.reasons.join("; "));
        check(d.c3.rd1Val === 0x5a && d.c3.rd2Val === 0xff && j.notes.some((n) => /0xff/.test(n) && /not gated/.test(n)), "decoder: C3 control 0x5A, 0x44 readback 0xFF recorded as a note");
        check(d.events.map((e) => e.code).join(",") === "1,2,6,7,8", "decoder: C3 events START, ARM_BEGIN, MCU_OPEN, MCU_CLOSE, ARM_END (no PRU)");
        const p3 = decodeBlock(dump("nominal-4-100"), L);
        check(p3.stops.length === 6 && p3.p3.every((e) => e.loadResp === L.COMMAND_SUCCESS && e.mcuStatus === 0), "decoder: P3 has six recorded stops and an ACKed load per pattern");
    }
    const t100 = decodeBlock(dump("nominal-2-100"), L).timing, t400 = decodeBlock(dump("nominal-2-400"), L).timing;
    check(t100.iepInc === 2500 && t100.tickClks === 500 && t100.sclTmoTicks === 10000 && t100.sclHz === 100000 && t100.freqSel === 3, "decoder: 100 kHz timing words 2500/500/10000/100000, selector 3");
    check(t400.iepInc === 680 && t400.tickClks === 136 && t400.sclTmoTicks === 36764 && t400.sclHz === 367647 && t400.freqSel === 2, "decoder: 400 kHz timing words 680/136/36764/367647, selector 2");

    // stuck PRU: the app aborted before the MCU driver; the gate must fail it
    for (const arm of [4, 5]) {
        const d = decodeBlock(dump(`stuck-${arm}`), L);
        const j = judgeArm(arm, d, { arm, hz: 100000, cookie: 9, r0Data }, L);
        check(!j.ok && d.state === L.RND91_STATE_ABORT && d.err === L.RND91_ERR_NOT_RELEASED, `gate: stuck-PRU arm ${arm} fails (ABORT, NOT_RELEASED)`);
        check(!d.events.some((e) => e.code === 6), `decoder: stuck-PRU arm ${arm} has no MCU_OPEN event`);
    }

    // ---- 3. gates on patched blocks --------------------------------------------------------------------
    const patch = (name, fn) => { const b = Buffer.from(dump(name)); fn(b); return b; };
    const w32 = (b, off, v) => b.writeUInt32LE(v >>> 0, off);
    const judgeBuf = (arm, b, extra = {}) => judgeArm(arm, decodeBlock(b, L), { arm, hz: 100000, cookie: cookieOf(arm), r0Data, ...extra }, L);

    // P1: unexpected ACK at 0x10 (a real C run of a device that ACKs it)
    const ack10 = judgeBuf(2, dump("ack10-2-100"));
    check(!ack10.ok && ack10.reasons.some((r) => /0x10/.test(r)), "gate: P1 with an unexpected ACK at 0x10 FAILS", ack10.reasons.join("; "));
    const nack61 = judgeBuf(2, patch("nominal-2-100", (b) => w32(b, L.RND91_OFF_PROBE, (0x61 << 16) | L.ADDRESS_ACKNOWLDEGE_FAILED)));
    check(!nack61.ok, "gate: P1 with a NACK at 0x61 FAILS");
    const odd21 = judgeBuf(2, patch("nominal-2-100", (b) => w32(b, L.RND91_OFF_PROBE + 16, (0x21 << 16) | 0x050e)));
    check(!odd21.ok, "gate: P1 with a TIME_OUT_ERROR response at 0x21 (neither ACK nor NACK) FAILS");
    // common fields
    check(!judgeBuf(1, patch("nominal-1-100", (b) => w32(b, L.RND91_OFF_REQ_COOKIE, 1))).ok, "gate: wrong cookie (stale block) FAILS");
    check(!judgeBuf(2, dump("nominal-1-100")).ok, "gate: block of another arm FAILS");
    check(!judgeBuf(1, patch("nominal-1-100", (b) => w32(b, L.RND91_OFF_MAGIC, 0))).ok, "gate: bad magic FAILS");
    check(!judgeBuf(1, patch("nominal-1-100", (b) => w32(b, L.RND91_OFF_STATE, L.RND91_STATE_RUNNING))).ok, "gate: state RUNNING (timeout) FAILS");
    check(!decodeBlock(Buffer.alloc(16), L).valid, "decoder: short block is invalid, not an exception");
    check(!judgeBuf(1, patch("nominal-1-100", (b) => w32(b, L.RND91_OFF_R0_STATUS, 0xffffffff))).ok, "gate: R0 MCU read status -1 FAILS");
    // ordering violations: MCU_OPEN between PRU_START and LINES_RELEASED
    const ev = (code, arg = 0) => ({ code, arg });
    check(checkOrdering([ev(1), ev(2, 4), ev(3), ev(6), ev(7), ev(4), ev(5)], L).length > 0, "ordering: MCU_OPEN inside the PRU window is reported");
    check(checkOrdering([ev(1), ev(6), ev(3), ev(5)], L).length > 0, "ordering: PRU_START while the MCU driver is open is reported");
    check(checkOrdering([ev(1), ev(3), ev(4)], L).length > 0, "ordering: PRU stopped but never released is reported");
    check(checkOrdering([ev(1), ev(3), ev(4), ev(5), ev(6), ev(7), ev(8)], L).length === 0, "ordering: a correct sequence is clean");
    const badOrder = patch("nominal-4-100", (b) => { // swap the first LINES_RELEASED and MCU_OPEN events of P3
        const idx = decodeBlock(b, L).events.map((e) => e.code);
        const i5 = idx.indexOf(5), i6 = idx.indexOf(6);
        const a = b.readUInt32LE(L.RND91_OFF_EV + 4 * i5), c2 = b.readUInt32LE(L.RND91_OFF_EV + 4 * i6);
        w32(b, L.RND91_OFF_EV + 4 * i5, c2); w32(b, L.RND91_OFF_EV + 4 * i6, a);
    });
    const jb = judgeBuf(4, badOrder);
    check(!jb.ok && jb.reasons.some((r) => /ordering/.test(r)), "gate: P3 block with MCU_OPEN before LINES_RELEASED FAILS on ordering", jb.reasons.join("; "));
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_STOP_CTRL, L.RND91_CTRL_RUNNING))).ok, "gate: PRU CONTROL still running at stop FAILS");
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_STOP_OUTCTRL, 1 << L.RND91_SCL_PIN))).ok, "gate: only SCL released at stop FAILS");
    // P2 / P3 / P4
    check(!judgeBuf(3, patch("nominal-3-100", (b) => { b[L.RND91_OFF_P2_DATA] ^= 1; })).ok, "gate: P2 data differing from R0 FAILS");
    check(!judgeBuf(3, dump("nominal-3-100"), { r0Data: null }).ok, "gate: P2 without R0 data FAILS");
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_P3 + 12, 0x54))).ok, "gate: P3 readback differing from the pattern FAILS");
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_P3 + 12, 0xff))).ok, "gate: P3 readback 0xFF (the silicon symptom) FAILS");
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_P3 + 8, 0xffffffff))).ok, "gate: P3 MCU readback status -1 FAILS (the readback is gated)");
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_P3_LOAD + 4, 0))).ok, "gate: P3 without the [0x22] load response FAILS");
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_P3_LOAD + 8, L.ADDRESS_ACKNOWLDEGE_FAILED))).ok, "gate: P3 with a NACKed load FAILS");
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_P3 + 4, L.ADDRESS_ACKNOWLDEGE_FAILED))).ok, "gate: P3 with a NACKed [0x11,p] write FAILS");
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_STOP_X_OUTCTRL + 4, 0))).ok, "gate: P3 phase-2 stop with the lines still driven FAILS");
    check(!judgeBuf(4, patch("nominal-4-100", (b) => w32(b, L.RND91_OFF_STOP_N, 9))).ok, "gate: more PRU stops than were recorded FAILS");
    // C3
    check(!judgeBuf(6, patch("nominal-6-100", (b) => w32(b, L.RND91_OFF_C3_RD1_VAL, 0xff))).ok, "gate: C3 control readback != 0x5A FAILS");
    check(!judgeBuf(6, patch("nominal-6-100", (b) => w32(b, L.RND91_OFF_C3_RD1, 0xffffffff))).ok, "gate: C3 control read status -1 FAILS");
    check(!judgeBuf(6, patch("nominal-6-100", (b) => w32(b, L.RND91_OFF_C3_OFF, 0xffffffff))).ok, "gate: C3 LEDs-off failure FAILS");
    check(judgeBuf(6, patch("nominal-6-100", (b) => { w32(b, L.RND91_OFF_C3_RD2_VAL, 0x00); w32(b, L.RND91_OFF_C3_RD2, 0xffffffff); })).ok, "gate: C3 0x44 readback of any value or status is only recorded");
    check(!judgeBuf(6, patch("nominal-6-100", (b) => w32(b, L.RND91_OFF_C3_N, 4))).ok, "gate: C3 that did not finish its steps FAILS");
    check(!judgeBuf(5, patch("nominal-5-100", (b) => { b[L.RND91_OFF_P4_RB2] ^= 1; })).ok, "gate: P4 readback 2 differing from the original FAILS");
    check(!judgeBuf(5, patch("nominal-5-100", (b) => { b[L.RND91_OFF_P4_RB1] ^= 1; })).ok, "gate: P4 readback 1 differing from the written bytes FAILS");
    check(!judgeBuf(5, patch("nominal-5-100", (b) => { for (let i = 0; i < 8; i++) b[L.RND91_OFF_P4_NEW + i] = b[L.RND91_OFF_P4_ORIG + i]; })).ok, "gate: P4 writing the original bytes (proves nothing) FAILS");
    check(!judgeBuf(5, patch("nominal-5-100", (b) => w32(b, L.RND91_OFF_P4_FLAGS, L.RND91_P4_F_WRITE_ISSUED))).ok, "gate: P4 write without a restore FAILS and says the EEPROM may hold new data");

    // ---- plan runner: order, stop at first failure -----------------------------------------------------
    const plan = armPlan(L);
    check(plan.map((s) => s.name).join(",") === "R0,P1@100k,P2@100k,C3,P3@100k,P4@100k,P1@400k,P2@400k,P3@400k,P4@400k", "plan: R0, P1, P2, C3, P3, P4 at 100k, then P1-P4 at 400k");
    const runnerFor = (mod) => { const calls = []; return { calls, run: async (step, cookie) => {
        calls.push(step.name);
        const k = step.hz / 1000;
        let b = Buffer.from(dump(`nominal-${step.arm}-${k}`));
        b.writeUInt32LE(cookie, L.RND91_OFF_REQ_COOKIE);
        if (mod) b = mod(step, b) || b;
        return b;
    } }; };
    const allOk = runnerFor(null);
    const rAll = await runPlan(allOk.run, { L });
    check(rAll.ok && rAll.results.every((x) => x.status === "PASS") && allOk.calls.length === 10, "plan: all ten arms pass on nominal blocks, run in order");
    const failP1 = runnerFor((step, b) => { if (step.name === "P1@100k") w32(b, L.RND91_OFF_PROBE, (0x61 << 16) | L.ADDRESS_ACKNOWLDEGE_FAILED); });
    const rP1 = await runPlan(failP1.run, { L });
    check(!rP1.ok && rP1.results[1].status === "FAIL" && rP1.results.slice(2).every((x) => x.status === "NOT_RUN") && failP1.calls.length === 2,
        "plan: the first failure (P1@100k) stops the run; later arms are NOT_RUN and never started");
    const failR0 = runnerFor((step, b) => { if (step.name === "R0") w32(b, L.RND91_OFF_R0_STATUS, 5); });
    const rR0 = await runPlan(failR0.run, { L });
    check(!rR0.ok && rR0.results[0].status === "FAIL" && failR0.calls.length === 1, "plan: a failed R0 baseline stops everything");
    const failC3 = runnerFor((step, b) => { if (step.name === "C3") w32(b, L.RND91_OFF_C3_RD1_VAL, 0xff); });
    const rC3 = await runPlan(failC3.run, { L });
    check(!rC3.ok && rC3.results[3].status === "FAIL" && failC3.calls.join(",") === "R0,P1@100k,P2@100k,C3" && rC3.results[4].status === "NOT_RUN", "plan: a failed C3 control stops the run before P3");
    const thrower = await runPlan(async () => { throw new Error("worker died"); }, { L });
    check(!thrower.ok && thrower.results[0].status === "ERROR" && thrower.results[1].status === "NOT_RUN", "plan: a runner error is ERROR and stops the run");
}

// ---- session launch gate ----------------------------------------------------------------------------
const tmpRuns = fs.mkdtempSync(path.join(os.tmpdir(), "rnd91-runs-"));
fs.mkdirSync(path.join(tmpRuns, "usedtag"));
const refuses = (o) => { try { return typeof session.refusal(o) === "string"; } catch (e) { return false; } };
const accepts = (o) => { try { return session.refusal(o) === null; } catch (e) { return false; } };
check(refuses({ dry: false, tag: undefined, runsDir: tmpRuns, holder: "1234" }), "session: refuses live without --live <tag>");
check(refuses({ dry: false, tag: "usedtag", runsDir: tmpRuns, holder: "1234" }), "session: refuses a tag that is not fresh");
check(refuses({ dry: false, tag: "../x", runsDir: tmpRuns, holder: "1234" }), "session: refuses a tag with path characters");
check(refuses({ dry: false, tag: "freshtag1", runsDir: tmpRuns, holder: undefined }), "session: refuses live outside waitrun (no lock holder)");
check(refuses({ dry: false, tag: "freshtag1", runsDir: tmpRuns, holder: "1234", boardsEnv: "am243,am263" }), "session: refuses when the boards are not am263 only");
check(accepts({ dry: false, tag: "freshtag1", runsDir: tmpRuns, holder: "1234", boardsEnv: "am263" }), "session: accepts a fresh tag under waitrun for am263");
check(accepts({ dry: true, tag: undefined }), "session: dry run needs no tag");

fs.rmSync(outDir, { recursive: true, force: true });
fs.rmSync(tmpRuns, { recursive: true, force: true });
console.log(`${ran} checks, ${failed} failed`);
process.exit(failed ? 1 : 0);
