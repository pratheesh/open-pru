// RND-91 CCS Scripting worker: one connection to the LP-AM263 R5F, then a file-command loop.
//   BOARD=am263 RND91_IPC=<dir> RND91_DEADLINE_MS=<ms> tools/scripting.cmd bench/rnd91_ccs.js   (started by rnd91_session.mjs)
// It does no judging. For every cmd-<n>.json in <dir>/ipc it halts the R5F, loads the image, clears the
// result block, writes the request words, runs until the app sets STATE to DONE or ABORT (or the arm
// times out), halts, and writes res-<n>.bin (the block) and res-<n>.json (what happened). It ends on
// <dir>/ipc/stop or at the deadline, with the R5F halted.
// Lab code is loaded from C:/t/tilab (TILAB_ROOT), not copied. Untested on a board.
const fs = require("fs");
const path = require("path");
const TILAB = (process.env.TILAB_ROOT || "C:/t/tilab").replace(/\\/g, "/");
const { sleep, hex, makeLog, board, disableAutoRun } = require(`${TILAB}/tools/lib.js`);

const b = board(process.env.BOARD || "am263");
if (b.id !== "am263") throw new Error(`rnd91_ccs: am263 only, got ${b.id}`);
const ipc = process.env.RND91_IPC;
if (!ipc) throw new Error("rnd91_ccs: RND91_IPC not set");
const deadline = Date.now() + Number(process.env.RND91_DEADLINE_MS || 1500000);
const log = makeLog("am263-rnd91");
const STATE_DONE = 2, STATE_ABORT = 3;

const ds = initScripting({ timeout: Math.max(60000, b.scriptTimeoutMs || 0) });
let r5 = null;
const w32 = (addr, v) => r5.memory.write(BigInt(addr), [v >>> 0], 32);
const r32 = (addr) => Number(BigInt(r5.memory.readOne(BigInt(addr), 32)) & 0xffffffffn);

function runCmd(cmd) {
    const out = { n: cmd.n, ok: false, reason: "", timedOut: false, pc: null, ms: 0 };
    const t0 = Date.now();
    try {
        try { r5.target.halt(); } catch (e) { /* already halted */ }
        r5.memory.loadProgram(cmd.image);
        // clear the block, then the request: a stale block from an earlier arm can never be mistaken for this one
        for (let off = 0; off < cmd.blockSize; off += 4) w32(cmd.blockAddr + off, 0);
        for (const [off, v] of cmd.request) w32(cmd.blockAddr + off, v);
        const back = r32(cmd.blockAddr + cmd.request[0][0]);
        if (back !== (cmd.request[0][1] >>> 0)) throw new Error(`request write did not read back (${hex(back)})`);
        r5.target.run(false);
        const end = Date.now() + cmd.timeoutMs;
        let st = 0;
        for (;;) {
            sleep(cmd.pollMs);
            st = r32(cmd.blockAddr + cmd.stateOff);
            if (st === STATE_DONE || st === STATE_ABORT) break;
            if (Date.now() > end) { out.timedOut = true; break; }
        }
        try { r5.target.halt(); } catch (e) { /* the app spins, so it is running; a failed halt is reported by the read */ }
        try { out.pc = hex(r5.expressions.evaluate("PC")); } catch (e) { /* not essential */ }
        const words = r5.memory.read(BigInt(cmd.blockAddr), cmd.blockSize / 4, 32);
        const buf = Buffer.alloc(cmd.blockSize);
        words.forEach((v, i) => buf.writeUInt32LE(Number(BigInt(v) & 0xffffffffn), 4 * i));
        fs.writeFileSync(path.join(ipc, `res-${cmd.n}.bin`), buf);
        out.ok = true;
        out.reason = out.timedOut ? `timeout, state ${st}` : `state ${st}`;
    } catch (e) {
        out.reason = String(e && e.message || e);
        try { r5.target.halt(); } catch (e2) { /* ignore */ }
    }
    out.ms = Date.now() - t0;
    fs.writeFileSync(path.join(ipc, `res-${cmd.n}.json`), JSON.stringify(out));
}

try {
    ds.configure(b.ccxml);
    r5 = ds.openSession(b.host.coreRe);
    r5.target.connect();
    disableAutoRun(r5);
    try { r5.target.halt(); } catch (e) { /* not running */ }
    log(`connected ${b.host.core}`);
    fs.writeFileSync(path.join(ipc, "ready"), String(Date.now()));
    let done = 0;
    while (Date.now() < deadline && !fs.existsSync(path.join(ipc, "stop"))) {
        const next = path.join(ipc, `cmd-${done + 1}.json`);
        if (fs.existsSync(next)) {
            const cmd = JSON.parse(fs.readFileSync(next, "utf8"));
            log(`cmd ${cmd.n}: ${cmd.image}`);
            runCmd(cmd);
            done++;
        } else {
            sleep(200);
        }
    }
    log(Date.now() >= deadline ? "deadline" : "stop");
} catch (e) {
    log(`FAIL: ${e.message || e}`);
    process.exitCode = 1;
} finally {
    try { if (r5) r5.target.halt(); } catch (e) { /* ignore */ }
    ds.shutdown();
}
