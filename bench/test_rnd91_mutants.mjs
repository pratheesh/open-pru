// Mutation check for the RND-91 bench tests: every mutant must be KILLED (test_rnd91.mjs exits non-zero on it).
//   node bench/test_rnd91_mutants.mjs
// A mutant is a text replacement in a temp copy of bench/ and of the app header. The replacement must match
// exactly once, otherwise the mutant is vacuous and this script fails. The unmutated copy must pass first.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..");
const APP = path.join(REPO, "examples/pru_i2c/mcuplus/am263x-lp/r5fss0-0_nortos");
const IFACE = path.join(REPO, "examples/pru_i2c/firmware/pru_i2c_interface.h");

const MUTANTS = [
    { id: "M1 swap SDA/SCL bits", file: "header", from: "#define RND91_SCL_PIN               (7U)\n#define RND91_SDA_PIN               (17U)", to: "#define RND91_SCL_PIN               (17U)\n#define RND91_SDA_PIN               (7U)" },
    { id: "M2 drop the R5F-after-PRU ordering guard", file: "header",
      from: "    if (g->owner != RND91_OWNER_NONE)\n    {\n        return Rnd91_guardRefuse(g, RND91_ERR_GUARD_OWNED);\n    }\n    g->owner = RND91_OWNER_MCU;",
      to: "    g->owner = RND91_OWNER_MCU;" },
    { id: "M3 session accepts an unexpected ACK at 0x10", file: "lib",
      from: "if (got.resp !== want) {", to: "if (got.resp !== want && !(e.addr === 0x10 && got.resp === R.ACK)) {" },
    { id: "M4 gates ignore the bus ordering log", file: "lib",
      from: "for (const b of checkOrdering(d.events, L)) reasons.push(`ordering: ${b}`);", to: "" },
    { id: "M5 P4 writes without having saved the original", file: "header",
      from: "saved = ((st == 0) && (resp == COMMAND_SUCCESS));", to: "saved = 1;" },
    { id: "M6 plan keeps going after a failure", file: "lib",
      from: "if (res.status !== \"PASS\") failed = true;", to: "" },
    { id: "M7 session launches live without a tag", file: "session",
      from: "if (!tag) return \"refusing to run live without --live <fresh tag>\";", to: "" },
    { id: "M8 400 kHz tick floor wrong (tLOW margin lost)", file: "header",
      from: "#define RND91_MIN_TICK_NS           (680U)", to: "#define RND91_MIN_TICK_NS           (500U)" },
    { id: "M9 'released' needs only one line", file: "header",
      from: "((outCtrl & RND91_PIN_MASK) == RND91_PIN_MASK);", to: "((outCtrl & RND91_PIN_MASK) != 0U);" },
    { id: "M10 R5F reads the MCU driver open before the PRU stop is checked", file: "header",
      from: "    err = Rnd91_guardPruStopped(&r->guard, ctrl, outc);\n    return (err == RND91_ERR_NONE) ? err : Rnd91_fail(r, err);",
      to: "    err = Rnd91_guardPruStopped(&r->guard, 0U, RND91_PIN_MASK);\n    return (err == RND91_ERR_NONE) ? err : Rnd91_fail(r, err);" },
    { id: "M11 P3 reverts to 0x44 then read", file: "header",
      from: "tx[0] = RND91_TPIC_CMD_SHIFT;", to: "tx[0] = RND91_TPIC_CMD_WR_NOW;" },
    { id: "M12 P3 skips the 0x22 load", file: "header",
      from: "(void)r->hal->pruWrite(r->ctx, RND91_ADDR_TPIC2810, ld, 1U, &ldResp);", to: "(void)ld;" },
    { id: "M13 C3 gates the 0x44 readback", file: "lib",
      from: "notes.push(`C3 0x44 readback (not gated)", to: "if (c.rd2Val !== 0xa5) reasons.push(\"C3: 0x44 readback\"); notes.push(`C3 0x44 readback (not gated)" },
];

function runSuite(root, header) {
    const r = spawnSync(process.execPath, [path.join(root, "bench/test_rnd91.mjs")], {
        encoding: "utf8", timeout: 300000, env: { ...process.env, RND91_HEADER: header, RND91_IFACE: IFACE },
    });
    const out = (r.stdout || "") + (r.stderr || "");
    return { code: r.status, fails: out.split(/\r?\n/).filter((l) => /^(FAIL|C    FAIL)/.test(l)), tail: out.trim().split(/\r?\n/).slice(-3).join(" | ") };
}

function makeCopy(mutant) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "rnd91-mut-"));
    fs.mkdirSync(path.join(root, "bench"), { recursive: true });
    for (const f of fs.readdirSync(HERE)) if (/\.(mjs|c|js)$/.test(f)) fs.copyFileSync(path.join(HERE, f), path.join(root, "bench", f));
    const header = path.join(root, "rnd91_bench.h");
    fs.copyFileSync(path.join(APP, "rnd91_bench.h"), header);
    if (mutant) {
        const target = { header, lib: path.join(root, "bench/rnd91_lib.mjs"), session: path.join(root, "bench/rnd91_session.mjs") }[mutant.file];
        const text = fs.readFileSync(target, "utf8").replace(/\r\n/g, "\n");
        const n = text.split(mutant.from).length - 1;
        if (n !== 1) throw new Error(`${mutant.id}: pattern matches ${n} times (must be exactly 1), the mutant would be vacuous`);
        fs.writeFileSync(target, text.replace(mutant.from, mutant.to));
    }
    return { root, header };
}

let bad = 0;
const base = makeCopy(null);
const b = runSuite(base.root, base.header);
console.log(`baseline (unmutated copy): exit ${b.code} - ${b.tail}`);
if (b.code !== 0) { console.log("FAIL: the unmutated copy does not pass, mutants prove nothing"); process.exit(1); }
fs.rmSync(base.root, { recursive: true, force: true });

for (const m of MUTANTS) {
    const c = makeCopy(m);
    const r = runSuite(c.root, c.header);
    const killed = r.code !== 0;
    if (!killed) bad++;
    console.log(`${killed ? "KILLED  " : "SURVIVED"} ${m.id}${killed ? ` (${r.fails.length} failing checks, first: ${(r.fails[0] || r.tail).slice(0, 110)})` : ""}`);
    fs.rmSync(c.root, { recursive: true, force: true });
}
console.log(`${MUTANTS.length} mutants, ${MUTANTS.length - bad} killed, ${bad} survived`);
process.exit(bad ? 1 : 0);
