#!/usr/bin/env python3
"""Check the editor's templates and write the lists the editor reads.

    python3 host/templates_index.py

editor/templates/*.json        shared templates (in the repository) -> editor/templates/index.json
editor/templates/local/*.json  your own (git ignores them)           -> editor/templates/local/index.json

Every template needs a .md of the same name next to it, saying how it is mapped. A template's
name carries its author ("OP-XY - blen2r", "OP-XY - blen2r alt 1"), so two people's versions for
the same device can live side by side. Exits non-zero if a template has a problem.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEMPLATES = ROOT / "editor" / "templates"
NAME_LEN = 28
TYPES = {"disabled", "cc", "note", "program", "pitchbend", "pressure", "base"}
SETTINGS = {"pages", "track_base", "follow_incoming", "cc_fader", "cc_fader_shift", "cc_mute", "cc_encoder",
            "solo_mutes", "cc_solo", "special_modes", "focus_rec", "focus_stop"}
NULLABLE = {"cc_fader_shift", "cc_mute", "cc_encoder", "cc_solo"}
OLD = {"cc_knob": "cc_encoder", "cc_focus": "focus_rec / focus_stop", "focus_select": "focus_stop"}


def problems(t, path):
    out = []
    for k in ("name", "device", "author"):
        if not isinstance(t.get(k), str) or not t[k]:
            out.append(f'missing "{k}"')
    name, author = t.get("name", ""), t.get("author", "")
    if author and f" - {author}" not in name:
        out.append(f'the name must contain " - {author}" (e.g. "OP-XY - {author}")')
    if len(name) > NAME_LEN or not name.isascii():
        out.append(f"the name must be ASCII, at most {NAME_LEN} characters")
    s = t.get("settings")
    if not isinstance(s, dict):
        return out + ['missing "settings"']
    for k in sorted(set(s) & set(OLD)):
        out.append(f'settings: "{k}" is from an older editor, now {OLD[k]} (save the template again)')
    for k in sorted(SETTINGS - set(s)):
        out.append(f'settings: missing "{k}"')
    for k in ("cc_fader", "cc_fader_shift", "cc_mute", "cc_encoder", "cc_solo"):
        v = s.get(k)
        if not (v is None and k in NULLABLE) and not (isinstance(v, int) and 0 <= v <= 127):
            out.append(f"settings: {k} must be 0..127" + (" or null" if k in NULLABLE else ""))
    for k in ("focus_rec", "focus_stop"):
        g = s.get(k)
        if not (isinstance(g, list) and len(g) == 8 and all(v is None or (isinstance(v, int) and 0 <= v <= 127) for v in g)):
            out.append(f"settings: {k} must be 8 values, each a CC 0..127 or null (one per encoder)")
    if not (isinstance(s.get("pages"), int) and 1 <= s["pages"] <= 8):
        out.append("settings: pages must be 1..8")
    tb = s.get("track_base")
    if not (isinstance(tb, list) and len(tb) == 8 and all(isinstance(x, int) and 0 <= x <= 15 for x in tb)):
        out.append("settings: track_base must be 8 numbers 0..15 (0 = track 1)")
    maps = t.get("mappings")
    if not isinstance(maps, list):
        return out + ['missing "mappings"']
    if len(maps) > 238:
        out.append("more than 238 mappings")
    for i, m in enumerate(maps):
        ok = (isinstance(m.get("control"), int) and 0 <= m["control"] <= 59 and
              isinstance(m.get("page"), int) and 0 <= m["page"] <= 8 and isinstance(m.get("shift"), bool) and
              m.get("type") in TYPES and isinstance(m.get("channel"), int) and 1 <= m["channel"] <= 16 and
              all(isinstance(m.get(k), int) and 0 <= m[k] <= 127 for k in ("number", "min", "max")) and
              isinstance(m.get("flags"), int) and 0 <= m["flags"] <= 0x3F)
        if not ok:
            out.append(f"mapping {i}: bad or missing field")
    if not path.with_suffix(".md").exists():
        out.append(f"no {path.with_suffix('.md').name} (how it is mapped)")
    return out


def index(folder):
    entries, bad, names = [], 0, {}
    for path in sorted(folder.glob("*.json")):
        if path.name == "index.json":
            continue
        try:
            t = json.loads(path.read_text())
        except ValueError as e:
            print(f"!! {path.relative_to(ROOT)}: not JSON ({e})")
            bad += 1
            continue
        p = problems(t, path)
        if t.get("name") in names:
            p.append(f'the name "{t["name"]}" is also used by {names[t["name"]]}')
        if p:
            bad += 1
            for x in p:
                print(f"!! {path.relative_to(ROOT)}: {x}")
            continue
        names[t["name"]] = path.name
        entries.append(dict(file=path.name, name=t["name"], device=t["device"], author=t["author"],
                            description=t.get("description", ""), doc=path.with_suffix(".md").name))
        print(f"   {path.relative_to(ROOT)}: {t['name']}")
    (folder / "index.json").write_text(json.dumps(entries, indent=1) + "\n")
    return bad


def main():
    bad = index(TEMPLATES)
    if (TEMPLATES / "local").is_dir():
        bad += index(TEMPLATES / "local")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
