#!/usr/bin/env python3
"""Offline checks, no mixer needed (QA.md, step 1):

    python3 host/selftest.py

- every template is valid and the editor's template lists are current (host/templates_index.py);
- the editor (editor/config.js, run with node) and the Python tools (host/mixer_cfg.py) encode
  the same configurations to the same bytes, and decode them back: the default, each template
  applied, the QA configuration;
- the firmware's header layout (mixer_config.h) still says what the editor and tools assume.
Needs node and Python 3 (no mido: mixer_cfg's MIDI part is not used).
"""
import json
import re
import subprocess
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))
sys.modules.setdefault("mido", types.ModuleType("mido"))     # only the format code is used
import mixer_cfg as M                                           # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    fails += 0 if ok else 1


def main():
    print("templates")
    r = subprocess.run([sys.executable, str(ROOT / "host/templates_index.py")], capture_output=True, text=True)
    print("".join("     " + line + "\n" for line in r.stdout.splitlines()), end="")
    check(r.returncode == 0, "templates_index.py: every template valid")
    st = subprocess.run(["git", "status", "--porcelain", "editor/templates/index.json"], cwd=ROOT,
                        capture_output=True, text=True).stdout.strip()
    check(not st or st.startswith("??"), "editor/templates/index.json was already current (else: commit it)")

    print("editor <-> Python, byte for byte")
    js = r"""
const c = require(process.argv[1] + '/editor/config.js'), fs = require('fs'), path = require('path');
const out = [];
const add = (what, cfg) => out.push({ what, cfg, bytes: Array.from(c.encodeConfig(cfg)) });
add('default', c.defaultConfig());
for (const dir of ['editor/templates', 'editor/templates/local']) {
  const d = path.join(process.argv[1], dir);
  if (!fs.existsSync(d)) continue;
  for (const f of fs.readdirSync(d).filter(f => f.endsWith('.json') && f !== 'index.json')) {
    const t = JSON.parse(fs.readFileSync(path.join(d, f)));
    const problems = c.checkTemplate(t);
    if (problems.length) { out.push({ what: `${dir}/${f}`, error: problems.join('; ') }); continue; }
    add(`${dir}/${f} applied`, c.applyTemplate(c.defaultConfig(), t));
  }
}
add('QA configuration', c.normalizeConfig(JSON.parse(fs.readFileSync(path.join(process.argv[1], 'host/qa/qa_config.json')))));
for (const o of out) if (o.bytes) o.back = c.decodeConfig(Uint8Array.from(o.bytes));
console.log(JSON.stringify(out));
"""
    try:
        res = subprocess.run(["node", "-e", js, str(ROOT)], capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        check(False, f"node could not run editor/config.js: {getattr(e, 'stderr', e)}")
        return
    for o in json.loads(res.stdout):
        if "error" in o:
            check(False, f"{o['what']}: {o['error']}")
            continue
        b = bytes(o["bytes"])
        check(M.encode(o["cfg"]) == b, f"{o['what']}: same bytes")
        check(M.decode(b) == o["back"], f"{o['what']}: same decoded configuration")
        check(o["back"] == o["cfg"], f"{o['what']}: the editor decodes what it encoded")

    print("firmware header")
    h = (ROOT / "fw/smcpad/examples/mixer/mixer_config.h").read_text()
    want = {"MCFG_VERSION": str(M.VERSION), "MCFG_HEADER": str(M.HEADER), "MCFG_ENTRY": str(M.ENTRY),
            "MCFG_NONE": hex(M.NONE), "MCFG_NAME_LEN": str(M.NAME_LEN)}
    for name, val in want.items():
        m = re.search(rf"#define {name}\s+(\S+)", h)
        check(bool(m) and m.group(1).lower() == val.lower(), f"{name} = {val}")

    print(f"\n{'all good' if not fails else f'{fails} failure(s)'}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
