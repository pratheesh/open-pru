// RND-91 bench session: LP-AM263 PRU1 I2C master on J8-73/J8-74 against the on-board I2C1 devices.
// A detached plain-code session (bench-run-template.md): it never calls a model, and every decision is
// written down here. Nothing in this file has been run against a board.
//
//   node bench/rnd91_session.mjs --dry-run [--build]          builds/checks every image and md5, exits 0, no probe
//   TILAB_DRY_RUN=1 node bench/rnd91_session.mjs              same
//   node tools/waitrun.mjs --boards am263 --expect-min 25 --status <dir> --owner "rnd91-bench (RND-91)" \
//        --preflight "node bench/rnd91_session.mjs --dry-run" -- node bench/rnd91_session.mjs --live <fresh tag>
//
// It refuses without `--live <fresh tag>` (a tag whose run directory does not exist yet) and refuses to run
// outside waitrun (no TILAB_LOCK_HOLDER = no probe lock). am263 only. Lab code is loaded from C:/t/tilab
// (TILAB_ROOT), not a copy.
//
// Arms, stopping at the first failure (see rnd91_lib.mjs armPlan/judgeArm):
//   R0 R5F-only read of 0x53:0x0400 | at 100 kHz then 400 kHz: P1 probe 0x61/0x52/0x53 ACK, 0x10/0x21 NACK;
//   P2 PRU read of the same 16 bytes vs R0; P3 TPIC2810 patterns 0x55/0xAA/0x00; P4 EEPROM write+restore.
// Each arm is one R5F run: the image is loaded, the block cleared, the request written, the core run until
// the app reports DONE/ABORT. Recovery: only if the JTAG scan fails before the first arm, one outlet-4
// power cycle (am263's own outlet, lab.json), under the lock waitrun holds.
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { spawnSync } from "node:child_process";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
import { REPO, loadLayout, armPlan, runPlan, decodeBlock } from "./rnd91_lib.mjs";

const TILAB = (process.env.TILAB_ROOT || "C:/t/tilab").replace(/\\/g, "/");
const BENCH = path.join(REPO, "bench");
const IMAGES = path.join(BENCH, "images");
const MANIFEST = path.join(BENCH, "rnd91_images.md5.json");
const RUNS = process.env.RND91_RUNS || "C:/t/rnd91-runs";
const R5F_BUILD = path.join(REPO, "examples/pru_i2c/mcuplus/am263x-lp/r5fss0-0_nortos/ti-arm-clang");
const PRU_BUILD = path.join(REPO, "examples/pru_i2c/firmware/am263x-lp/icss_m0_pru1_fw/ti-pru-cgt");
const IMAGE_SPECS = [
    { key: "r5f", staged: "rnd91_am263_r5f.out", built: path.join(R5F_BUILD, "pru_i2c.release.out") },
    { key: "pru1", staged: "rnd91_am263_pru1.out", built: path.join(PRU_BUILD, "generated/pru_i2c_am263x-lp_icss_m0_pru1_fw.out") },
    { key: "pru1_load_bin_h", staged: "rnd91_am263_pru1_load_bin.h", built: path.join(REPO, "examples/pru_i2c/firmware/am263x-lp/pru1_load_bin.h") },
];
const MAX_RUNTIME_MS = 25 * 60 * 1000;      // whole session, connect included (the AM263x GEL connect is ~62 s)
const ARM_TIMEOUT_MS = 30 * 1000;           // one arm: the app is short, an arm longer than this is a hang
const FRESH_TAG = /^[A-Za-z0-9][A-Za-z0-9._-]{3,40}$/;

const args = process.argv.slice(2);
const flag = (n) => args.includes(n);
const opt = (n) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : undefined; };
const dry = flag("--dry-run") || process.env.TILAB_DRY_RUN === "1";
const tag = opt("--live");
const log = (m) => console.log(`[rnd91 ${new Date().toISOString()}] ${m}`);
const md5 = (f) => crypto.createHash("md5").update(fs.readFileSync(f)).digest("hex");

// ---- gate: refuse before touching anything -----------------------------------------------------
export function refusal({ dry, tag, runsDir = RUNS, holder = process.env.TILAB_LOCK_HOLDER, boardsEnv = process.env.TILAB_BOARDS } = {}) {
    if (dry) return null;
    if (!tag) return "refusing to run live without --live <fresh tag>";
    if (!FRESH_TAG.test(tag)) return `--live tag "${tag}" is not a plain tag (${FRESH_TAG})`;
    if (fs.existsSync(path.join(runsDir, tag))) return `--live tag "${tag}" is not fresh: ${path.join(runsDir, tag)} exists`;
    if (!holder) return "refusing to run live outside waitrun: no TILAB_LOCK_HOLDER, so no probe lock is held";
    if (boardsEnv && boardsEnv !== "am263") return `refusing: this session drives am263 only, TILAB_BOARDS=${boardsEnv}`;
    return null;
}

// ---- images --------------------------------------------------------------------------------------
function build() {
    const gmake = "C:/ti/ccs2100/ccs/utils/bin/gmake.exe";
    for (const [cwd, target] of [[PRU_BUILD, []], [R5F_BUILD, ["pru_i2c.release.out"]]]) {
        const r = spawnSync(gmake, target, { cwd, encoding: "utf8", env: { ...process.env, PATH: `C:/ti/ccs2100/ccs/utils/bin;${process.env.PATH}` } });
        if (r.status !== 0) throw new Error(`build failed in ${cwd}:\n${(r.stdout || "").slice(-1500)}\n${(r.stderr || "").slice(-1500)}`);
    }
}

function stageAndCheck({ updateManifest = false } = {}) {
    fs.mkdirSync(IMAGES, { recursive: true });
    const manifest = fs.existsSync(MANIFEST) ? JSON.parse(fs.readFileSync(MANIFEST, "utf8")) : {};
    const got = {};
    const problems = [];
    for (const s of IMAGE_SPECS) {
        const dst = path.join(IMAGES, s.staged);
        if (fs.existsSync(s.built)) fs.copyFileSync(s.built, dst);
        if (!fs.existsSync(dst)) { problems.push(`${s.key}: no image at ${dst} (build with --build)`); continue; }
        got[s.key] = md5(dst);
        if (!updateManifest && manifest[s.key] !== got[s.key]) {
            problems.push(`${s.key}: md5 ${got[s.key]} != manifest ${manifest[s.key] || "(none)"}`);
        }
    }
    if (updateManifest) fs.writeFileSync(MANIFEST, JSON.stringify(got, null, 2) + "\n");
    return { md5: got, problems };
}

// ---- lab code (lablib from C:/t/tilab) -------------------------------------------------------------
async function loadLab() {
    const mod = await import(pathToFileURL(`${TILAB}/tools/lablib.mjs`).href);
    const b = mod.board("am263");
    if (b.probeSerial !== "S26B0004") throw new Error(`lab.json am263 probeSerial ${b.probeSerial}, expected S26B0004`);
    return { mod, b };
}

function checkBoardFacts(b, L) {
    const bad = [];
    if (b.icss.base !== "0x48000000") bad.push(`icss.base ${b.icss.base}`);
    if (b.icss.pruClockMHz !== 200) bad.push(`pruClockMHz ${b.icss.pruClockMHz}`);
    if (b.power?.outlet !== 4) bad.push(`power.outlet ${b.power?.outlet}`);
    if (Number(b.msramScratch) !== L.RND91_BLOCK_ADDR) bad.push(`msramScratch ${b.msramScratch} != block 0x${L.RND91_BLOCK_ADDR.toString(16)}`);
    return bad;
}

// ---- live ------------------------------------------------------------------------------------------
async function live(L, mod, b, outDir, images) {
    const t0 = Date.now();
    const ipc = path.join(outDir, "ipc");
    fs.mkdirSync(ipc, { recursive: true });
    const events = [];
    const ev = (m) => { events.push({ at: new Date().toISOString(), msg: m }); log(m); };
    const done = { tag, board: "am263", startedAt: new Date(t0).toISOString(), images: images.md5, status: "ERROR", arms: [], events };
    const writeDone = (status, extra = {}) => {
        Object.assign(done, { status, endedAt: new Date().toISOString(), runtimeS: Math.round((Date.now() - t0) / 1000), ...extra });
        fs.writeFileSync(path.join(outDir, "done.json"), JSON.stringify(done, null, 2));
    };
    let worker = null;
    try {
        // JTAG scan first; one outlet-4 power cycle only if it fails (lab rules: am263's own outlet, its probe lock held).
        let scan;
        try { mod.preflight(b); scan = mod.jtagScan(b); } catch (e) { scan = { ok: false, out: String(e.message || e) }; }
        ev(`jtag scan: ${scan.ok ? "ok" : "FAILED"}${scan.irBits ? ` (${scan.irBits} bits)` : ""}`);
        if (!scan.ok) {
            const r = mod.systemReset(b, { heldIds: ["am263"] });
            ev(`recovery: power cycle of outlet ${b.power.outlet}: ${r.ok ? "ok" : "FAILED " + r.error}`);
            if (!r.ok) { writeDone("FAIL", { error: "JTAG scan failed and the recovery power cycle did not bring the board back" }); return 1; }
        }
        // CCS worker: one connection, then a file-command loop
        const script = path.join(BENCH, "rnd91_ccs.js");
        const deadlineMs = Math.max(60000, MAX_RUNTIME_MS - (Date.now() - t0) - 60000);
        worker = mod.runScripting(script, { BOARD: "am263", RND91_IPC: ipc, RND91_DEADLINE_MS: String(deadlineMs), TILAB_ROOT: TILAB }, path.join(outDir, "ccs.log"));
        const readyBy = Date.now() + (b.scriptTimeoutMs || 240000);
        while (!fs.existsSync(path.join(ipc, "ready"))) {
            if (Date.now() > readyBy) throw new Error("CCS worker never connected (no ready file)");
            await new Promise((r) => setTimeout(r, 500));
        }
        ev("CCS worker connected");

        let n = 0;
        const runArm = async (step, cookie) => {
            n++;
            const cmd = {
                n, image: path.join(IMAGES, IMAGE_SPECS[0].staged).replace(/\\/g, "/"),
                blockAddr: L.RND91_BLOCK_ADDR, blockSize: L.RND91_BLOCK_SIZE, stateOff: L.RND91_OFF_STATE,
                timeoutMs: ARM_TIMEOUT_MS, pollMs: 100,
                request: [[L.RND91_OFF_REQ_MAGIC, L.RND91_REQ_MAGIC], [L.RND91_OFF_REQ_ARM, step.arm],
                    [L.RND91_OFF_REQ_HZ, step.hz], [L.RND91_OFF_REQ_COOKIE, cookie]],
            };
            fs.writeFileSync(path.join(ipc, `cmd-${n}.json`), JSON.stringify(cmd));
            ev(`arm ${step.name}: requested (cookie 0x${cookie.toString(16)})`);
            const resJson = path.join(ipc, `res-${n}.json`);
            const until = Date.now() + ARM_TIMEOUT_MS + 60000;
            while (!fs.existsSync(resJson)) {
                if (Date.now() > until) throw new Error("no result from the CCS worker");
                await new Promise((r) => setTimeout(r, 200));
            }
            const res = JSON.parse(fs.readFileSync(resJson, "utf8"));
            if (!res.ok) throw new Error(`worker: ${res.reason}`);
            const bin = path.join(ipc, `res-${n}.bin`);
            const keep = path.join(outDir, `arm-${String(n).padStart(2, "0")}-${step.name}.bin`);
            fs.copyFileSync(bin, keep);
            return fs.readFileSync(bin);
        };
        const r = await runPlan(runArm, { L });
        done.arms = r.results.map(({ decoded, ...rest }) => rest);
        for (const a of r.results) ev(`arm ${a.name}: ${a.status}${a.reasons && a.reasons.length ? " - " + a.reasons.join("; ") : ""}`);
        writeDone(r.ok ? "PASS" : "FAIL");
        return r.ok ? 0 : 1;
    } catch (e) {
        ev(`ERROR: ${e.message || e}`);
        writeDone("ERROR", { error: String(e.message || e) });
        return 2;
    } finally {
        fs.writeFileSync(path.join(ipc, "stop"), "");
        if (worker) await Promise.race([worker, new Promise((r) => setTimeout(r, 60000))]);
    }
}

// ---- main ------------------------------------------------------------------------------------------
async function main() {
    const why = refusal({ dry, tag });
    if (why) { console.error(`rnd91_session: ${why}`); return 2; }
    const L = loadLayout();
    if (flag("--build")) build();
    const images = stageAndCheck({ updateManifest: flag("--update-manifest") });
    const { mod, b } = await loadLab();
    const factBad = checkBoardFacts(b, L);
    const plan = armPlan(L);
    log(`board am263: probe ${b.probeSerial}, ICSSM ${b.icss.base}, PRU ${b.icss.pruClockMHz} MHz, outlet ${b.power.outlet}, block 0x${L.RND91_BLOCK_ADDR.toString(16)}`);
    for (const [k, v] of Object.entries(images.md5)) log(`image ${k}: md5 ${v}`);
    log(`plan: ${plan.map((s) => s.name).join(" ")}`);
    const problems = [...images.problems, ...factBad.map((x) => `lab.json: ${x}`)];
    if (dry) {
        for (const p of problems) console.error(`DRY-RUN PROBLEM: ${p}`);
        log(problems.length ? "dry run: FAILED, no probe was opened" : "dry run: ok, no probe was opened");
        return problems.length ? 1 : 0;
    }
    if (problems.length) { for (const p of problems) console.error(`refusing: ${p}`); return 2; }
    const outDir = path.join(RUNS, tag);
    fs.mkdirSync(outDir, { recursive: false });
    return live(L, mod, b, outDir, images);
}

const isMain = process.argv[1] && pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url;
if (isMain) {
    main().then((c) => process.exit(c), (e) => { console.error(e); process.exit(2); });
}
