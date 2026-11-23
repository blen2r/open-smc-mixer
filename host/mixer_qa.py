#!/usr/bin/env python3
"""Guided QA of the SMC-Mixer firmware and web editor, over USB-MIDI.

    python3 host/mixer_qa.py                # editor round trip, then the hardware checks
    python3 host/mixer_qa.py --no-editor    # load the QA configuration directly, skip the editor part
    python3 host/mixer_qa.py --from 12      # start the hardware checks at check 12

1. Your current configuration is saved to work/qa_backup_<time>.json (and restored at the end).
2. Editor: you import host/qa/qa_config.json in the web editor, make five listed changes (in both modes) and save.
   The script reads the configuration back and checks it is exactly the QA one plus the changes.
3. Hardware: the script asks for one action at a time ("turn knob 1 right") and checks the MIDI the
   mixer sends on USB. It knows which control you touched (editor events), so a wrong control or
   page is reported as such. A check passes on its own once the expected messages came; press
   Enter to end one early, "s" + Enter to skip it.

Keep the OP-XY (or any Bluetooth host) disconnected: the checks send test messages. Start with no
strip soloed and no Rec / Select focus lit (those change what the base profile sends).
"""
import argparse
import json
import os
import select
import sys
import time

import mido

from mixer_cfg import Mixer, decode, encode, normalize, unpack7

HERE = os.path.dirname(os.path.abspath(__file__))
QA_CONFIG = os.path.join(HERE, "qa", "qa_config.json")
WORK = os.path.join(os.path.dirname(HERE), "work")

GREEN, RED, YEL, DIM, BOLD, OFF = "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[1m", "\033[0m"

# controls (editor/config.js CONTROLS)
KNOB = lambda k: k - 1                      # 1-based names, as printed on the mixer
FADER = lambda f: 8 + f - 1
STRIP = lambda s, row: 16 + (s - 1) * 4 + ["mute", "solo", "rec", "select"].index(row)
BTN = {n: 48 + i for i, n in enumerate(["play", "pause", "record", "rewind", "ff", "<<", ">>",
                                        "up", "down", "left", "right"])}
BTN["shift"] = 60


def control_name(c):
    if c < 8:
        return f"Knob {c + 1}"
    if c < 16:
        return f"Fader {c - 7}"
    if c < 48:
        b = c - 16
        return f"Strip {b // 4 + 1} {['Mute', 'Solo', 'Rec', 'Select'][b % 4]}"
    names = ["Play", "Pause", "Record", "Rewind", "Fast-forward", "<<", ">>", "Up", "Down", "Left",
             "Right", "Bluetooth", "Shift"]
    return names[c - 48] if c - 48 < len(names) else f"control {c}"


def describe(m):
    t, ch = m[0] & 0xF0, (m[0] & 15) + 1
    d1 = m[1] if len(m) > 1 else 0
    d2 = m[2] if len(m) > 2 else 0
    return {0x80: f"note off ch{ch} {d1}", 0x90: f"note on ch{ch} {d1} vel {d2}",
            0xB0: f"CC ch{ch} #{d1} = {d2}", 0xC0: f"program ch{ch} {d1}",
            0xD0: f"pressure ch{ch} {d1}", 0xE0: f"pitch bend ch{ch} {d2 << 7 | d1}"}.get(
        t, " ".join(f"{b:02x}" for b in m))


# ---- message predicates (channels 1-based, as in the editor)

def cc(ch, num, lo=0, hi=127, value=None):
    def p(m):
        return (len(m) == 3 and m[0] == 0xB0 | (ch - 1) and m[1] == num and lo <= m[2] <= hi
                and (value is None or m[2] == value))
    p.text = f"CC ch{ch} #{num}" + (f" = {value}" if value is not None else
                                    f" ({lo}..{hi})" if (lo, hi) != (0, 127) else "")
    return p


def note_on(ch, num, vel=None):
    def p(m):
        return len(m) == 3 and m[0] == 0x90 | (ch - 1) and m[1] == num and m[2] > 0 and (vel is None or m[2] == vel)
    p.text = f"note on ch{ch} {num}" + (f" vel {vel}" if vel is not None else "")
    return p


def note_off(ch, num):
    def p(m):
        return len(m) == 3 and m[1] == num and (m[0] == 0x80 | (ch - 1) or (m[0] == 0x90 | (ch - 1) and m[2] == 0))
    p.text = f"note off ch{ch} {num}"
    return p


def program(ch, lo=0, hi=127, value=None):
    def p(m):
        return len(m) == 2 and m[0] == 0xC0 | (ch - 1) and lo <= m[1] <= hi and (value is None or m[1] == value)
    p.text = f"program ch{ch}" + (f" {value}" if value is not None else f" ({lo}..{hi})")
    return p


def pitchbend(ch):
    def p(m):
        return len(m) == 3 and m[0] == 0xE0 | (ch - 1)
    p.text = f"pitch bend ch{ch}"
    return p


def pressure(ch):
    def p(m):
        return len(m) == 2 and m[0] == 0xD0 | (ch - 1)
    p.text = f"pressure ch{ch}"
    return p


def any_of(*ps):
    def p(m):
        return any(q(m) for q in ps)
    p.text = " or ".join(q.text for q in ps)
    return p


# ---- the editor part

EDITOR_STEPS = """\
In the editor ({bold}host/mixer_editor.sh{off}, Chrome):
  1. Connect (USB), then {bold}Import{off} host/qa/qa_config.json and {bold}Save to mixer{off}.
  2. Make these five changes (Mode bar on {bold}CC{off}, page "All pages", layer Normal, unless said):
     a. Click {bold}Knob 1{off}, change the CC number 20 -> {bold}22{off}.
     b. Click {bold}Strip 4 Mute{off} (marked disabled), click {bold}Remove mapping{off}.
     c. Click {bold}Down{off}: Type CC, channel 1, CC number {bold}52{off}, Button {bold}toggle{off}.
     d. Settings: {bold}Pages 3{off}.
     e. Mode bar on {bold}DAW{off}: click {bold}Down{off}: Type CC, channel 1, CC number {bold}71{off} (momentary).
  3. {bold}Save to mixer{off}. (Leave the editor open; you can tick "highlight controls" and watch the
     monitor during the hardware checks.)
"""


def expected_config():
    cfg = json.load(open(QA_CONFIG))
    ms = cfg["mappings"]
    next(m for m in ms if m["control"] == KNOB(1))["number"] = 22
    cfg["mappings"] = [m for m in ms if m["control"] != STRIP(4, "mute")]
    cfg["mappings"].append(dict(mode="cc", control=BTN["down"], page=0, shift=False, type="cc", channel=1, number=52,
                                flags=1, min=0, max=127))
    cfg["mappings"].append(dict(mode="daw", control=BTN["down"], page=0, shift=False, type="cc", channel=1,
                                number=71, flags=0, min=0, max=127))
    cfg["pages"] = 3
    return cfg


def mapping_key(m):
    return (m.get("mode", "cc"), m["control"], m["page"], m["shift"])


def mapping_text(m):
    layer = (f"{m.get('mode', 'cc').upper()}, {'page ' + str(m['page']) if m['page'] else 'all pages'}"
             f"{' + Shift' if m['shift'] else ''}")
    return (f"{control_name(m['control'])} [{layer}]: {m['type']} ch{m['channel']} #{m['number']} "
            f"flags {m['flags']} range {m['min']}..{m['max']}")


def compare(got, want):
    """The differences between two configurations, as lines (empty: identical)."""
    out = []
    for k in want:
        if k != "mappings" and got.get(k) != want[k]:
            out.append(f"setting {k}: mixer has {got.get(k)!r}, expected {want[k]!r}")
    g = {mapping_key(m): m for m in got["mappings"]}
    w = {mapping_key(m): m for m in want["mappings"]}
    if len(g) != len(got["mappings"]):
        out.append("the mixer has two mappings for the same control and layer")
    for k in sorted(set(g) | set(w)):
        a, b = g.get(k), w.get(k)
        if a and not b:
            out.append(f"unexpected mapping: {mapping_text(a)}")
        elif b and not a:
            out.append(f"missing mapping: {mapping_text(b)}")
        elif any(a[f] != b[f] for f in ("type", "channel", "number", "flags", "min", "max")):
            out.append(f"mapping differs:\n      mixer:    {mapping_text(a)}\n      expected: {mapping_text(b)}")
    return out


def looks_like_qa(cfg):
    """Most of the QA mappings are there (the QA configuration, with or without the editor changes)."""
    cfg = normalize(cfg) if "base" in cfg else cfg
    qa = {mapping_key(m) for m in json.load(open(QA_CONFIG))["mappings"]}
    return len(qa & {mapping_key(m) for m in cfg["mappings"]}) >= len(qa) // 2


def strip_version(cfg):
    cfg = dict(cfg)
    cfg.pop("version", None)
    return cfg


# ---- the hardware checks

def C(say, ctl=None, need=(), allow=None, silent=False, page=None, shift=None, prep=None, ask=None,
      exact=None, unordered=False, after=None):
    """One check. need: messages that must come, in this order. allow: what every message must match
    (default: any of need). silent: nothing may be sent. exact: the number of messages expected.
    unordered: need in any order. after: a function returning problems, run once the check passed."""
    if allow is None and need:
        allow = any_of(*need)
    return dict(say=say, ctl=ctl, need=list(need), allow=allow, silent=silent, page=page, shift=shift,
                prep=prep, ask=ask, exact=exact, unordered=unordered, after=after)


def checks(qa):
    def stored_mode(want):
        def f():
            got = qa.m.hello().get("mode")
            cfg_mode = decode(qa.m.read())["mode"]
            out = []
            if got != want:
                out.append(f"the mixer says it is in {got} mode, expected {want}")
            if cfg_mode != want:
                out.append(f"the stored configuration says {cfg_mode} mode, expected {want} (not kept)")
            return out
        return f

    def pickup_prep():
        input(f"   Put {BOLD}fader 3 all the way down{OFF}, then press Enter ")
        time.sleep(2)                       # past the echo window of the fader's own last values
        qa.send([0xB5, 30, 64])             # CC 30 ch 6 = 64: the fader is now below its value
        print(f"   {DIM}(sent CC 30 = 64 on ch 6 to the mixer){OFF}")

    return [
        C("Press Shift + Rewind twice (to be sure you are on page 1). Then press Enter.", ctl=BTN["rewind"],
          silent=True, shift=True),
        # knobs
        C("Turn knob 1 to the right a little.", ctl=KNOB(1), page=1, need=[cc(2, 22)]),
        C("Turn knob 2 a lot to the right (a few turns).", ctl=KNOB(2), page=1,
          need=[cc(2, 21, value=50)], allow=cc(2, 21, 40, 50)),
        C("Turn knob 2 a lot to the left.", ctl=KNOB(2), page=1,
          need=[cc(2, 21, value=40)], allow=cc(2, 21, 40, 50)),
        C("Turn knob 3 a little (page 1: base profile, CC 12 of track 3).", ctl=KNOB(3), page=1,
          need=[cc(3, 12)]),
        C("Turn knob 4 to the right a little.", ctl=KNOB(4), page=1, need=[pitchbend(4)]),
        C("Turn knob 5 (disabled: must send nothing). Then press Enter.", ctl=KNOB(5), silent=True),
        # faders
        C("Move fader 1 (no Shift).", ctl=FADER(1), page=1, need=[cc(1, 7)]),
        C("Hold Shift and move fader 1 (the macro: CC 13 ch 12).", ctl=FADER(1), shift=True,
          need=[cc(12, 13)]),
        C("Move fader 2.", ctl=FADER(2), need=[pressure(5)]),
        C("Pickup: slowly push fader 3 all the way up.", ctl=FADER(3), prep=pickup_prep,
          need=[cc(6, 30, 64, 127), cc(6, 30, value=127)], allow=cc(6, 30, 64, 127),
          ask=("Before you moved it, was the LED above fader 3 lit (solid)? And is it off now?")),
        C("Move fader 4 (page 1: base profile, volume of track 4).", ctl=FADER(4), page=1, need=[cc(4, 7)]),
        # buttons
        C("Press Strip 1 Mute twice (toggle: 127, then 0).", ctl=STRIP(1, "mute"),
          need=[cc(1, 50, value=127), cc(1, 50, value=0)], exact=2,
          ask="Was the Strip 1 Mute LED lit after the first press, and off after the second?"),
        C("Press and release Strip 2 Solo (momentary note 36 ch 10, velocity 100).", ctl=STRIP(2, "solo"),
          need=[note_on(10, 36, 100), note_off(10, 36)], exact=2),
        C("Press Strip 4 Mute twice (mapping removed in the editor: base mute of track 4, on then off).",
          ctl=STRIP(4, "mute"), need=[cc(4, 9, value=127), cc(4, 9, value=0)], exact=2, unordered=True),
        C("Press and release Pause (trigger: CC 51 = 100 on press only).", ctl=BTN["pause"],
          need=[cc(1, 51, value=100)], exact=1),
        C("Press Record (program change 5).", ctl=BTN["record"], need=[program(1, value=5)], exact=1),
        C("Press and release Play (base explicitly: note 94).", ctl=BTN["play"],
          need=[note_on(1, 94), note_off(1, 94)], exact=2),
        C("Press Down twice (added in the editor: toggle CC 52).", ctl=BTN["down"],
          need=[cc(1, 52, value=127), cc(1, 52, value=0)], exact=2),
        C("Press and release Up, without Shift (base: note 96).", ctl=BTN["up"], shift=False,
          need=[note_on(1, 96), note_off(1, 96)], exact=2),
        C("Hold Shift, press and release Up (trigger: note 61 on and off at once).", ctl=BTN["up"],
          shift=True, need=[note_on(1, 61), note_off(1, 61)], exact=2),
        # pages
        C("Press Shift + Fast-forward once (page 2). Then press Enter.", ctl=BTN["ff"], silent=True,
          shift=True, ask="Did the page indicator blink?"),
        C("Turn knob 3 to the right a little (page 2: program changes 0..10, ch 3).",
          ctl=KNOB(3), page=2, need=[program(3, 0, 10)], allow=program(3, 0, 10)),
        C("Move fader 4 over its whole travel (page 2: CC 40 ch 8, range 20..100).", ctl=FADER(4), page=2,
          need=[cc(8, 40, value=20), cc(8, 40, value=100)], allow=cc(8, 40, 20, 100)),
        C("Move fader 1 (page 2: volume of track 9).", ctl=FADER(1), page=2, need=[cc(9, 7)]),
        C("Press Shift + Fast-forward once more (page 3, the editor's Pages change). Then press Enter.",
          ctl=BTN["ff"], silent=True, shift=True),
        C("Move fader 1 (page 3: volume of track 1).", ctl=FADER(1), page=3, need=[cc(1, 7)]),
        C("Press Shift + Rewind twice, back to page 1. Then press Enter.", ctl=BTN["rewind"], silent=True,
          shift=True),
        # modes
        C("Hold Shift and press Left (DAW mode). Keep holding Shift a second, then let go and press Enter.",
          ctl=BTN["left"], silent=True, shift=True, after=stored_mode("daw"),
          ask="While Shift was held, was the Left arrow lit (and Right off)?"),
        C("Move fader 1 (DAW mode: pitch bend ch 1, with the touch note 104).", ctl=FADER(1),
          need=[note_on(1, 104), pitchbend(1)], allow=any_of(pitchbend(1), note_on(1, 104), note_off(1, 104))),
        C("Turn knob 1 to the right a little (DAW mode: V-Pot, CC 16 relative).", ctl=KNOB(1),
          need=[cc(1, 16, 1, 63)]),
        C("Press and release Strip 1 Mute (DAW mode: note 16; the CC-mode mapping must not apply).",
          ctl=STRIP(1, "mute"), need=[note_on(1, 16), note_off(1, 16)], exact=2),
        C("Press and release Down (a DAW-mode mapping from the editor: CC 71, momentary).", ctl=BTN["down"],
          need=[cc(1, 71, value=127), cc(1, 71, value=0)], exact=2),
        C("Press and release Up (a DAW-mode mapping from the QA file: CC 70, momentary).", ctl=BTN["up"],
          need=[cc(1, 70, value=127), cc(1, 70, value=0)], exact=2),
        C("Hold Shift and press Right (back to CC mode). Then press Enter.", ctl=BTN["right"], silent=True,
          shift=True, after=stored_mode("cc"), ask="While Shift was held, was the Right arrow lit?"),
        C("Press Down twice (CC mode again: toggle CC 52).", ctl=BTN["down"],
          need=[cc(1, 52, value=127), cc(1, 52, value=0)], exact=2),
    ]


class QA:
    def __init__(self, mixer):
        self.m = mixer

    def send(self, b):
        self.m.o.send(mido.Message.from_bytes(b))

    def drain(self):
        for _ in self.m.i.iter_pending():
            pass

    def poll(self):
        """(midi messages, events) received since the last call."""
        msgs, evs = [], []
        for msg in self.m.i.iter_pending():
            if msg.type == "sysex":
                d = list(msg.data)
                if d[:3] == [0x7D, 0x4D, 0x10]:
                    ctl, vh, vl, page, shift = unpack7(d[4:])[:5]
                    evs.append((ctl, vh << 8 | vl, page, shift))
                continue
            msgs.append(tuple(msg.bytes()))
        return msgs, evs

    def run_check(self, n, total, c):
        print(f"\n{BOLD}[{n}/{total}]{OFF} {c['say']}")
        if c["prep"]:
            c["prep"]()
        self.drain()
        msgs, evs = [], []
        start = last = time.time()
        skipped = early = False
        while True:
            r, _, _ = select.select([sys.stdin], [], [], 0.02)
            if r:
                line = sys.stdin.readline().strip().lower()
                if line == "s":
                    skipped = True
                    break
                early = True
                break
            m, e = self.poll()
            for x in m:
                print(f"   {DIM}<- {describe(x)}{OFF}")
            for x in e:
                if x[0] != c["ctl"] and x[0] != BTN["shift"]:
                    print(f"   {YEL}(touched {control_name(x[0])}, page {x[2]}{', Shift' if x[3] else ''}){OFF}")
            if m or e:
                last = time.time()
            msgs += m
            evs += e
            ctl_seen = c["ctl"] is None or any(x[0] == c["ctl"] for x in evs)
            if not c["silent"] and ctl_seen and self.needs_met(c, msgs) and time.time() - last > 1.0:
                break
            if time.time() - start > 90:
                break
        if skipped:
            print(f"   {YEL}skipped{OFF}")
            return None
        problems = self.evaluate(c, msgs, evs)
        if not problems and c["after"]:
            problems += c["after"]()
        if not problems and c["ask"]:
            a = input(f"   {c['ask']} [y/n] ").strip().lower()
            if not a.startswith("y"):
                problems.append(f"you answered no to: {c['ask']}")
        if problems:
            for p in problems:
                print(f"   {RED}FAIL{OFF} {p}")
            return problems
        print(f"   {GREEN}PASS{OFF}")
        return []

    @staticmethod
    def needs_met(c, msgs):
        if c["unordered"]:
            return all(any(p(m) for m in msgs) for p in c["need"])
        i = 0
        for m in msgs:
            if i < len(c["need"]) and c["need"][i](m):
                i += 1
        return i == len(c["need"])

    def evaluate(self, c, msgs, evs):
        out = []
        mine = [x for x in evs if x[0] == c["ctl"]]
        if c["ctl"] is not None and not mine:
            out.append(f"no event from {control_name(c['ctl'])}"
                       + (f" (saw {', '.join(sorted({control_name(x[0]) for x in evs}))})" if evs else ""))
            return out
        if c["page"] is not None and mine and mine[0][2] != c["page"]:
            out.append(f"the mixer was on page {mine[0][2]}, expected page {c['page']}")
        if c["shift"] is not None and mine and not any(bool(x[3]) == c["shift"] for x in mine):
            out.append(f"Shift was {'not ' if c['shift'] else ''}held")
        if c["silent"]:
            if msgs:
                out.append(f"expected no MIDI, got {len(msgs)}: " + ", ".join(describe(m) for m in msgs[:4]))
            return out
        bad = [m for m in msgs if not c["allow"](m)]
        if bad:
            out.append(f"{len(bad)} unexpected message(s), e.g. " + ", ".join(describe(m) for m in bad[:4])
                       + f" (expected {c['allow'].text})")
        if not self.needs_met(c, msgs):
            out.append("did not get, in order: " + "; ".join(p.text for p in c["need"])
                       + (f" (got {len(msgs)} message(s){': ' + ', '.join(describe(m) for m in msgs[-4:]) if msgs else ''})"))
        if c["exact"] is not None and len(msgs) != c["exact"]:
            out.append(f"expected {c['exact']} message(s), got {len(msgs)}")
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-editor", action="store_true", help="load the expected configuration directly")
    ap.add_argument("--from", dest="start", type=int, default=1, help="first hardware check")
    args = ap.parse_args()

    m = Mixer()
    h = m.hello()
    print(f"mixer: firmware {h['fw']}, configuration {h['length']} bytes, page {h['page']}")
    os.makedirs(WORK, exist_ok=True)
    current = decode(m.read())
    if looks_like_qa(current):
        # a QA run left its configuration on the mixer: yours is the latest backup that is not QA
        olds = [os.path.join(WORK, f) for f in sorted(os.listdir(WORK)) if f.startswith("qa_backup_")]
        olds = [f for f in olds if not looks_like_qa(json.load(open(f)))]
        if not olds:
            sys.exit("the mixer holds the QA configuration and there is no backup of yours in work/")
        backup = olds[-1]
        original = normalize(json.load(open(backup)))
        print(f"the mixer still holds the QA configuration; yours is {backup}")
    else:
        backup = os.path.join(WORK, time.strftime("qa_backup_%Y%m%d_%H%M%S.json"))
        original = current
        json.dump(original, open(backup, "w"), indent=1)
        print(f"your configuration is saved in {backup}")

    want = expected_config()
    if args.no_editor:
        st = m.write(encode(want))
        print("QA configuration loaded" if st == 0 else f"{RED}the mixer refused it (status {st}){OFF}")
    else:
        print("\n" + EDITOR_STEPS.format(bold=BOLD, off=OFF))
        while True:
            input("Press Enter once you saved in the editor ")
            diffs = compare(decode(m.read()), want)
            if not diffs:
                print(f"{GREEN}PASS{OFF} editor: the mixer holds exactly the QA configuration with your four changes")
                break
            print(f"{RED}FAIL{OFF} editor: the configuration on the mixer differs:")
            for d in diffs:
                print("   " + d)
            a = input("[r]etry after fixing it in the editor, [l]oad the expected one and go on, [q]uit? ").strip().lower()
            if a.startswith("l"):
                m.write(encode(want))
                break
            if a.startswith("q"):
                return restore(m, original)

    qa = QA(m)
    m.cmd(0x10, b"\x01")                     # control events, to know what was touched
    results = []
    try:
        cs = checks(qa)
        print(f"\n{BOLD}Hardware checks{OFF}: Enter ends a check early, s + Enter skips it.")
        for n, c in enumerate(cs, 1):
            if n < args.start:
                continue
            results.append((n, c["say"], qa.run_check(n, len(cs), c)))
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        m.cmd(0x10, b"\x00")

    passed = sum(1 for _, _, r in results if r == [])
    failed = [(n, s, r) for n, s, r in results if r]
    skipped = sum(1 for _, _, r in results if r is None)
    print(f"\n{BOLD}Summary{OFF}: {GREEN}{passed} passed{OFF}, {RED}{len(failed)} failed{OFF}, {skipped} skipped")
    for n, s, r in failed:
        print(f"  [{n}] {s}")
        for p in r:
            print(f"       {p}")
    restore(m, original)


def restore(m, original):
    a = input("\nRestore your previous configuration? [Y/n] ").strip().lower()
    if not a.startswith("n"):
        st = m.write(encode(original))
        print("restored" if st == 0 else f"{RED}restore refused (status {st}){OFF}")


if __name__ == "__main__":
    main()
