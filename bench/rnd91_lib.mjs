// RND-91 bench library: result block decoder, bus-ordering check, per-arm gates and the arm plan.
// Pure code, no probe, no lab access. The offsets are read from the R5F app's own header
// (examples/pru_i2c/mcuplus/am263x-lp/r5fss0-0_nortos/rnd91_bench.h) and the PRU response codes from
// examples/pru_i2c/firmware/pru_i2c_interface.h, so there is one copy of every number.
// Nothing here has been run against a board.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const REPO = path.resolve(HERE, "..");
export const APP_DIR = path.join(REPO, "examples/pru_i2c/mcuplus/am263x-lp/r5fss0-0_nortos");
export const HEADER = process.env.RND91_HEADER || path.join(APP_DIR, "rnd91_bench.h");
export const IFACE = process.env.RND91_IFACE || path.join(REPO, "examples/pru_i2c/firmware/pru_i2c_interface.h");

// "#define NAME (0x1234U)" or "(12U)" -> number. Only plain numeric defines are taken.
export function parseDefines(file) {
    const out = {};
    for (const line of fs.readFileSync(file, "utf8").split(/\r?\n/)) {
        const m = /^\s*#define\s+([A-Za-z_]\w*)\s+\(\s*(0[xX][0-9a-fA-F]+|\d+)[uU]?\s*\)/.exec(line);
        if (m) out[m[1]] = Number(m[2]);
    }
    return out;
}

export function loadLayout() {
    const L = { ...parseDefines(IFACE), ...parseDefines(HEADER) };
    // The one derived value: the pin mask is an expression in the header, (1 << SCL) | (1 << SDA).
    if ("RND91_SCL_PIN" in L && "RND91_SDA_PIN" in L) L.RND91_PIN_MASK = ((1 << L.RND91_SCL_PIN) | (1 << L.RND91_SDA_PIN)) >>> 0;
    // Every L.<name> this file uses must exist, or a gate would compare against undefined and pass silently.
    const used = new Set([...fs.readFileSync(fileURLToPath(import.meta.url), "utf8").matchAll(/L\.([A-Za-z_]\w*)/g)].map((m) => m[1]));
    const missing = [...used].filter((k) => !(k in L));
    if (missing.length) throw new Error(`rnd91_lib: not found in ${HEADER} / ${IFACE}: ${missing.join(", ")}`);
    return L;
}

export const ARM_NAMES = { 1: "R0", 2: "P1", 3: "P2", 4: "P3", 5: "P4" };
export const EV_NAMES = {
    1: "START", 2: "ARM_BEGIN", 3: "PRU_START", 4: "PRU_STOPPED", 5: "LINES_RELEASED",
    6: "MCU_OPEN", 7: "MCU_CLOSE", 8: "ARM_END", 9: "REFUSED",
};

// ---- decoder -----------------------------------------------------------------------------------
// buf: Buffer of at least RND91_BLOCK_SIZE bytes (the block read over JTAG). Never throws on a bad
// block: it returns { valid: false, why }.
export function decodeBlock(buf, L = loadLayout()) {
    if (!Buffer.isBuffer(buf) || buf.length < L.RND91_BLOCK_SIZE) {
        return { valid: false, why: `block is ${buf ? buf.length : 0} bytes, need ${L.RND91_BLOCK_SIZE}` };
    }
    const w = (off) => buf.readUInt32LE(off);
    const bytes = (off, n) => Array.from(buf.subarray(off, off + n));
    if (w(L.RND91_OFF_MAGIC) !== L.RND91_MAGIC) return { valid: false, why: `magic 0x${w(L.RND91_OFF_MAGIC).toString(16)}` };
    if (w(L.RND91_OFF_VERSION) !== L.RND91_VERSION) return { valid: false, why: `version ${w(L.RND91_OFF_VERSION)}` };
    if (w(L.RND91_OFF_CANARY) !== L.RND91_CANARY) return { valid: false, why: `canary 0x${w(L.RND91_OFF_CANARY).toString(16)}` };
    const evCount = w(L.RND91_OFF_EV_COUNT);
    const events = [];
    for (let i = 0; i < Math.min(evCount, L.RND91_EV_MAX); i++) {
        const v = w(L.RND91_OFF_EV + 4 * i);
        events.push({ code: v >>> 24, arg: v & 0xffffff });
    }
    const stopN = w(L.RND91_OFF_STOP_N);
    const stops = [];
    for (let i = 0; i < Math.min(stopN, L.RND91_STOP_MAX); i++) {
        stops.push({ ctrl: w(L.RND91_OFF_STOP_CTRL + 4 * i), outCtrl: w(L.RND91_OFF_STOP_OUTCTRL + 4 * i) });
    }
    const probeN = Math.min(w(L.RND91_OFF_PROBE_N), L.RND91_PROBE_MAX);
    const probe = [];
    for (let i = 0; i < probeN; i++) {
        const v = w(L.RND91_OFF_PROBE + 4 * i);
        probe.push({ addr: v >>> 16, resp: v & 0xffff });
    }
    const p3n = Math.min(w(L.RND91_OFF_P3_N), L.RND91_P3_MAX);
    const p3 = [];
    for (let i = 0; i < p3n; i++) {
        const e = L.RND91_OFF_P3 + L.RND91_P3_STRIDE * i;
        p3.push({ pattern: w(e), pruResp: w(e + 4), mcuStatus: w(e + 8) | 0, value: w(e + 12) });
    }
    return {
        valid: true,
        state: w(L.RND91_OFF_STATE),
        stage: w(L.RND91_OFF_STAGE),
        req: { magic: w(L.RND91_OFF_REQ_MAGIC), arm: w(L.RND91_OFF_REQ_ARM), hz: w(L.RND91_OFF_REQ_HZ), cookie: w(L.RND91_OFF_REQ_COOKIE) },
        err: w(L.RND91_OFF_ERR),
        timing: {
            iepInc: w(L.RND91_OFF_IEP_INC), tickClks: w(L.RND91_OFF_TICK_CLKS), sclTmoTicks: w(L.RND91_OFF_SCL_TMO_TICKS),
            freqSel: w(L.RND91_OFF_FREQ_SEL), sclHz: w(L.RND91_OFF_SCL_HZ),
        },
        evCount, events, stops, probe,
        r0: { status: w(L.RND91_OFF_R0_STATUS) | 0, data: bytes(L.RND91_OFF_R0_DATA, 16) },
        p2: { resp: w(L.RND91_OFF_P2_RESP), data: bytes(L.RND91_OFF_P2_DATA, 16) },
        p3,
        p4: {
            origResp: w(L.RND91_OFF_P4_ORIG_RESP), orig: bytes(L.RND91_OFF_P4_ORIG, 8),
            wrResp: w(L.RND91_OFF_P4_WR_RESP), newData: bytes(L.RND91_OFF_P4_NEW, 8),
            rb1Status: w(L.RND91_OFF_P4_RB1_STATUS) | 0, rb1: bytes(L.RND91_OFF_P4_RB1, 8),
            rstResp: w(L.RND91_OFF_P4_RST_RESP),
            rb2Status: w(L.RND91_OFF_P4_RB2_STATUS) | 0, rb2: bytes(L.RND91_OFF_P4_RB2, 8),
            flags: w(L.RND91_OFF_P4_FLAGS),
        },
    };
}

// ---- bus ordering ------------------------------------------------------------------------------
// The PRU and the MCU I2C1 controller share the two bus wires. From the event log: the MCU may be
// open only outside [PRU_START, LINES_RELEASED], and the PRU may start only while the MCU is closed.
// A REFUSED event means the app's guard stopped a handover; that is reported too (it is a failed arm).
export function checkOrdering(events, L = loadLayout()) {
    const bad = [];
    let pruOwns = false;
    let mcuOpen = false;
    events.forEach((e, i) => {
        const n = EV_NAMES[e.code] || `EV${e.code}`;
        if (e.code === 3) { // PRU_START
            if (mcuOpen) bad.push(`event ${i}: PRU_START while the MCU I2C1 driver is open`);
            if (pruOwns) bad.push(`event ${i}: PRU_START while the PRU already owns the bus`);
            pruOwns = true;
        } else if (e.code === 5) { // LINES_RELEASED
            if (!pruOwns) bad.push(`event ${i}: LINES_RELEASED without a PRU_START`);
            pruOwns = false;
        } else if (e.code === 4) { // PRU_STOPPED
            if (!pruOwns) bad.push(`event ${i}: PRU_STOPPED without a PRU_START`);
        } else if (e.code === 6) { // MCU_OPEN
            if (pruOwns) bad.push(`event ${i}: MCU_OPEN before the PRU was stopped and released`);
            if (mcuOpen) bad.push(`event ${i}: MCU_OPEN twice`);
            mcuOpen = true;
        } else if (e.code === 7) { // MCU_CLOSE
            if (!mcuOpen) bad.push(`event ${i}: MCU_CLOSE without MCU_OPEN`);
            mcuOpen = false;
        } else if (e.code === 9) {
            bad.push(`event ${i}: guard refused a handover (error ${e.arg})`);
        } else if (!EV_NAMES[e.code]) {
            bad.push(`event ${i}: unknown code ${n}`);
        }
    });
    if (pruOwns) bad.push("log ends with the PRU still owning the bus (no LINES_RELEASED)");
    if (mcuOpen) bad.push("log ends with the MCU I2C1 driver still open");
    return bad;
}

// ---- per-arm gates -----------------------------------------------------------------------------
// Every judge returns { ok, reasons[], notes[] }. ctx: { arm, hz, cookie, r0Data }.
const hexb = (a) => a.map((b) => b.toString(16).padStart(2, "0")).join("");
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// The PRU response words the gates expect (from pru_i2c_interface.h)
export function responses(L = loadLayout()) {
    return { ACK: L.COMMAND_SUCCESS, NACK: L.ADDRESS_ACKNOWLDEGE_FAILED };
}

// Expected probe outcome, P1: three devices ACK, two addresses NACK.
export const P1_EXPECT = [
    { addr: 0x61, ack: true }, { addr: 0x52, ack: true }, { addr: 0x53, ack: true },
    { addr: 0x10, ack: false }, { addr: 0x21, ack: false },
];

export function judgeCommon(d, ctx, L = loadLayout()) {
    const reasons = [];
    if (!d.valid) return { ok: false, reasons: [`block invalid: ${d.why}`], notes: [] };
    if (d.req.cookie !== ctx.cookie) reasons.push(`cookie 0x${d.req.cookie.toString(16)} != 0x${ctx.cookie.toString(16)} (stale block?)`);
    if (d.req.arm !== ctx.arm) reasons.push(`arm ${d.req.arm} != requested ${ctx.arm}`);
    if (d.req.hz !== ctx.hz) reasons.push(`hz ${d.req.hz} != requested ${ctx.hz}`);
    if (d.state !== L.RND91_STATE_DONE) reasons.push(`state ${d.state}, want DONE (${L.RND91_STATE_DONE}); err ${d.err}`);
    if (d.err !== 0) reasons.push(`app error ${d.err}`);
    if (d.evCount > L.RND91_EV_MAX) reasons.push(`event log overflow (${d.evCount} > ${L.RND91_EV_MAX})`);
    for (const b of checkOrdering(d.events, L)) reasons.push(`ordering: ${b}`);
    d.stops.forEach((s, i) => {
        const running = (s.ctrl & (L.RND91_CTRL_ENABLE | L.RND91_CTRL_RUNNING)) !== 0;
        const released = (s.outCtrl & L.RND91_PIN_MASK) === L.RND91_PIN_MASK;
        if (running || !released) reasons.push(`stop ${i}: CONTROL 0x${s.ctrl.toString(16)} OUT_CTRL 0x${s.outCtrl.toString(16)} (not stopped and released)`);
    });
    return { ok: reasons.length === 0, reasons, notes: [] };
}

export function judgeArm(armId, d, ctx, L = loadLayout()) {
    const base = judgeCommon(d, ctx, L);
    if (!d.valid) return base;
    const reasons = [...base.reasons];
    const notes = [...base.notes];
    const R = responses(L);
    switch (armId) {
        case 1: // R0
            if (d.r0.status !== 0) reasons.push(`R0: MCU read status ${d.r0.status}`);
            notes.push(`R0 data ${hexb(d.r0.data)}`);
            break;
        case 2: { // P1
            if (d.probe.length !== P1_EXPECT.length) reasons.push(`P1: ${d.probe.length} probe results, want ${P1_EXPECT.length}`);
            for (const e of P1_EXPECT) {
                const got = d.probe.find((p) => p.addr === e.addr);
                if (!got) { reasons.push(`P1: no result for 0x${e.addr.toString(16)}`); continue; }
                const want = e.ack ? R.ACK : R.NACK;
                if (got.resp !== want) {
                    reasons.push(`P1: 0x${e.addr.toString(16)} response 0x${got.resp.toString(16)}, want ${e.ack ? "ACK" : "NACK"} 0x${want.toString(16)}`);
                }
            }
            break;
        }
        case 3: // P2
            if (d.p2.resp !== R.ACK) reasons.push(`P2: PRU read response 0x${d.p2.resp.toString(16)}`);
            if (!ctx.r0Data) reasons.push("P2: no R0 data to compare with");
            else if (!eq(d.p2.data, ctx.r0Data)) reasons.push(`P2: PRU read ${hexb(d.p2.data)} != R0 ${hexb(ctx.r0Data)}`);
            notes.push(`P2 data ${hexb(d.p2.data)}`);
            break;
        case 4: { // P3
            const want = [0x55, 0xaa, 0x00];
            if (d.p3.length !== want.length) reasons.push(`P3: ${d.p3.length} entries, want ${want.length}`);
            d.p3.forEach((e, i) => {
                if (e.pattern !== want[i]) reasons.push(`P3[${i}]: pattern 0x${e.pattern.toString(16)}, want 0x${want[i].toString(16)}`);
                if (e.pruResp !== R.ACK) reasons.push(`P3[${i}]: PRU write response 0x${e.pruResp.toString(16)}`);
                if (e.mcuStatus !== 0) {
                    // The TPIC2810 read is documented only by the SDK example (i2c_led_read_write_lld); no datasheet was read.
                    notes.push(`P3[${i}]: MCU readback failed (status ${e.mcuStatus}): visual only`);
                } else if (e.value !== want[i]) {
                    reasons.push(`P3[${i}]: readback 0x${e.value.toString(16)}, wrote 0x${want[i].toString(16)}`);
                }
            });
            break;
        }
        case 5: { // P4
            const p = d.p4;
            if (p.origResp !== R.ACK) reasons.push(`P4: save of the original, response 0x${p.origResp.toString(16)}`);
            if (ctx.r0Data && !eq(p.orig, ctx.r0Data.slice(0, 8))) reasons.push(`P4: original ${hexb(p.orig)} != R0 ${hexb(ctx.r0Data.slice(0, 8))}`);
            if (eq(p.newData, p.orig)) reasons.push("P4: the pattern written equals the original (a readback proves nothing)");
            if (p.wrResp !== R.ACK) reasons.push(`P4: write response 0x${p.wrResp.toString(16)}`);
            if (p.rb1Status !== 0 || !eq(p.rb1, p.newData)) reasons.push(`P4: readback 1 status ${p.rb1Status} ${hexb(p.rb1)}, want ${hexb(p.newData)}`);
            if (p.rstResp !== R.ACK) reasons.push(`P4: restore response 0x${p.rstResp.toString(16)}`);
            if (p.rb2Status !== 0 || !eq(p.rb2, p.orig)) reasons.push(`P4: readback 2 status ${p.rb2Status} ${hexb(p.rb2)}, want original ${hexb(p.orig)}`);
            if ((p.flags & L.RND91_P4_F_WRITE_ISSUED) && !(p.flags & L.RND91_P4_F_RESTORE_ISSUED)) {
                reasons.push(`P4: EEPROM MAY HOLD THE NEW DATA: restore not issued; original at 0x53:0x0400 was ${hexb(p.orig)}`);
            }
            break;
        }
        default:
            reasons.push(`unknown arm ${armId}`);
    }
    return { ok: reasons.length === 0, reasons, notes };
}

// ---- the plan ----------------------------------------------------------------------------------
// R0 once (the baseline that proves the bus), then P1-P4 at 100 kHz, then P1-P4 at 400 kHz.
export function armPlan(L = loadLayout()) {
    const plan = [{ name: "R0", arm: L.RND91_ARM_R0, hz: 100000 }];
    for (const hz of [100000, 400000]) {
        for (const [name, arm] of [["P1", L.RND91_ARM_P1], ["P2", L.RND91_ARM_P2], ["P3", L.RND91_ARM_P3], ["P4", L.RND91_ARM_P4]]) {
            plan.push({ name: `${name}@${hz / 1000}k`, arm, hz });
        }
    }
    return plan;
}

// Run the plan in order and stop at the first failure; later arms are NOT_RUN. runArm(step, cookie) must
// return the raw block Buffer. The judge sees R0's data as the reference for P2 and P4.
export async function runPlan(runArm, { L = loadLayout(), plan = armPlan(L), judge = judgeArm, cookieBase = 0x52000000 } = {}) {
    const results = [];
    let r0Data = null;
    let failed = false;
    for (let i = 0; i < plan.length; i++) {
        const step = plan[i];
        if (failed) { results.push({ name: step.name, status: "NOT_RUN" }); continue; }
        const cookie = (cookieBase + i + 1) >>> 0;
        let res;
        try {
            const buf = await runArm({ ...step, cookie }, cookie);
            const d = decodeBlock(buf, L);
            res = judge(step.arm, d, { arm: step.arm, hz: step.hz, cookie, r0Data }, L);
            if (res.ok && step.arm === L.RND91_ARM_R0) r0Data = d.r0.data;
            res = { name: step.name, status: res.ok ? "PASS" : "FAIL", reasons: res.reasons, notes: res.notes, decoded: d.valid ? d : undefined };
        } catch (e) {
            res = { name: step.name, status: "ERROR", reasons: [String(e && e.message || e)] };
        }
        results.push(res);
        if (res.status !== "PASS") failed = true;
    }
    return { ok: !failed, results };
}

export function md5(file) {
    return fs.existsSync(file) ? import("node:crypto").then((c) => c.createHash("md5").update(fs.readFileSync(file)).digest("hex")) : Promise.resolve(null);
}
