#!/usr/bin/env python3
"""SMC-Mixer configuration over USB-MIDI (the editor protocol of docs/mixer-config.md), from the
command line. The web editor (editor/) speaks the same protocol; this is its test bench.

    python3 host/mixer_cfg.py hello
    python3 host/mixer_cfg.py dump                 # the active configuration as JSON
    python3 host/mixer_cfg.py load cfg.json        # write + commit
    python3 host/mixer_cfg.py factory
    python3 host/mixer_cfg.py events               # live control events (Ctrl-C)
"""
import json
import struct
import sys
import time
import zlib

import mido

MAGIC = b"SMXC"
VERSION = 2
HEADER = 64
ENTRY = 10
NONE = 0x80
NAME_LEN = 28
TYPES = ["disabled", "cc", "note", "program", "pitchbend", "pressure", "base"]
MODES = ["daw", "cc"]
HEAD_FMT = "<4sHHBBBBBBBBB8sBBBBBBB28sI"      # editor/config.js, mixer_config.h


def pack7(data):
    out = []
    for i in range(0, len(data), 7):
        g = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << k for k, b in enumerate(g)))
        out += [b & 0x7F for b in g]
    return out


def unpack7(data):
    out = []
    i = 0
    while i < len(data):
        msb = data[i]
        i += 1
        for k in range(7):
            if i >= len(data):
                break
            out.append(data[i] | (((msb >> k) & 1) << 7))
            i += 1
    return bytes(out)


class Mixer:
    def __init__(self):
        name = next((n for n in mido.get_input_names() if "SMC-Mixer open fw" in n), None)
        if not name:
            sys.exit("SMC-Mixer (our firmware) not found on USB-MIDI")
        self.i = mido.open_input(name)
        self.o = mido.open_output(next(n for n in mido.get_output_names() if "SMC-Mixer open fw" in n))
        self.seq = 0x40                     # away from the editor's, which starts at 1

    def cmd(self, c, data=b"", timeout=2.0):
        self.seq = (self.seq + 1) & 0x7F
        # Replies go to every client of the port: drop what arrived while we were not listening
        # (the web editor's replies, whose sequence numbers can match ours).
        for _ in self.i.iter_pending():
            pass
        self.o.send(mido.Message("sysex", data=[0x7D, 0x4D, c, self.seq] + pack7(bytes(data))))
        end = time.time() + timeout
        while time.time() < end:
            for m in self.i.iter_pending():
                d = list(m.data) if m.type == "sysex" else []
                if d[:3] == [0x7D, 0x4D, 0x7F] and d[3] == self.seq:
                    return d[4], unpack7(d[5:])
            time.sleep(0.002)
        raise SystemExit(f"no reply to command {c:#x}")

    def hello(self):
        st, d = self.cmd(0x01)
        fw, cv, mx, ctl, pages, page, length = struct.unpack("<HHHBBBH", d[:11])
        return dict(fw=fw, config_version=cv, max_bytes=mx, controls=ctl, pages=pages, page=page, length=length,
                    mode=MODES[d[11]] if len(d) > 11 and d[11] < 2 else None)

    def read(self):
        h = self.hello()
        blob = b""
        while len(blob) < h["length"]:
            n = min(128, h["length"] - len(blob))
            st, d = self.cmd(0x02, struct.pack("<HB", len(blob), n))
            if st:
                raise SystemExit(f"read failed, status {st}")
            blob += d
        return blob

    def write(self, blob):
        for off in range(0, len(blob), 96):
            st, _ = self.cmd(0x03, struct.pack("<H", off) + blob[off:off + 96])
            if st:
                raise SystemExit(f"write failed at {off}, status {st}")
        st, _ = self.cmd(0x04, struct.pack("<H", len(blob)), timeout=5)
        return st


def decode(blob):
    (magic, ver, n, mode, takeover, led, follow, remember, accel, a3, a2, pages, tb,
     ccf, ccfs, ccm, cck, ccfo, flags, _, name, _crc) = struct.unpack_from(HEAD_FMT, blob)
    assert magic == MAGIC, magic
    if ver != VERSION:
        raise SystemExit(f"configuration version {ver}: update the mixer's firmware")
    cc = lambda b: None if b & NONE else b
    cfg = dict(mode=MODES[mode], fader_takeover=["jump", "pickup"][takeover],
               fader_led=bool(led), follow_incoming=bool(follow), remember_values=bool(remember),
               accel=bool(accel), accel_ms_x3=a3, accel_ms_x2=a2, pages=pages, track_base=list(tb),
               name=name.split(b"\0")[0].decode("ascii", "replace"),
               cc_fader=cc(ccf), cc_fader_shift=cc(ccfs), cc_mute=cc(ccm), cc_knob=cc(cck), cc_focus=cc(ccfo),
               solo_mutes=bool(flags & 1), special_modes=bool(flags & 2), mappings=[])
    for i in range(n):
        c, layer, t, ch, num, fl, lo, hi = blob[HEADER + i * ENTRY: HEADER + i * ENTRY + 8]
        cfg["mappings"].append(dict(mode="daw" if layer & 0x20 else "cc", control=c, page=layer & 15,
                                    shift=bool(layer & 16), type=TYPES[t], channel=ch + 1, number=num,
                                    flags=fl, min=lo, max=hi))
    return cfg


def normalize(cfg):
    """A configuration in the current shape: older exports (base: opxy / daw, no mode on mappings,
    no CC profile) are brought up to date, as the editor's normalizeConfig()."""
    cfg = dict(cfg)
    base = cfg.pop("base", None)
    out = dict(mode="daw" if base == "daw" else "cc" if base else "daw", name="OP-XY - blen2r" if base != "daw" else "",
               cc_fader=7, cc_fader_shift=10, cc_mute=9, cc_knob=12, cc_focus=12, solo_mutes=True, special_modes=True)
    out.update(cfg)
    out.pop("version", None)
    out["mappings"] = [dict({"mode": "daw" if base == "daw" else "cc"}, **m) for m in cfg.get("mappings", [])]
    return out


def encode(cfg):
    cc = lambda v: NONE if v is None else v & 127
    name = cfg.get("name", "").encode("ascii", "replace")[:NAME_LEN].ljust(NAME_LEN, b"\0")
    maps = cfg["mappings"]
    head = bytearray(struct.pack(
        HEAD_FMT, MAGIC, VERSION, len(maps), MODES.index(cfg["mode"]),
        ["jump", "pickup"].index(cfg["fader_takeover"]), int(cfg["fader_led"]), int(cfg["follow_incoming"]),
        int(cfg["remember_values"]), int(cfg["accel"]), cfg["accel_ms_x3"], cfg["accel_ms_x2"], cfg["pages"],
        bytes(cfg["track_base"]), cc(cfg["cc_fader"]), cc(cfg["cc_fader_shift"]), cc(cfg["cc_mute"]),
        cc(cfg["cc_knob"]), cc(cfg["cc_focus"]), int(cfg["solo_mutes"]) | int(cfg["special_modes"]) << 1, 0,
        name, 0))
    ents = b"".join(bytes([m["control"], m["page"] | (16 if m["shift"] else 0) | (0x20 if m.get("mode") == "daw" else 0),
                           TYPES.index(m["type"]), m["channel"] - 1, m["number"], m["flags"], m["min"], m["max"], 0, 0])
                    for m in maps)
    struct.pack_into("<I", head, 60, zlib.crc32(bytes(head) + ents))
    return bytes(head) + ents


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    m = Mixer()
    c = sys.argv[1]
    if c == "hello":
        print(m.hello())
    elif c == "dump":
        print(json.dumps(decode(m.read()), indent=1))
    elif c == "load":
        cfg = normalize(json.load(open(sys.argv[2])))
        st = m.write(encode(cfg))
        print("committed" if st == 0 else f"refused, status {st}")
    elif c == "factory":
        print("status", m.cmd(0x06, timeout=5)[0])
    elif c == "events":
        m.cmd(0x10, b"\x01")
        try:
            for msg in m.i:
                d = list(msg.data) if msg.type == "sysex" else []
                if d[:3] == [0x7D, 0x4D, 0x10]:
                    ctl, vh, vl, page, shift = unpack7(d[4:])
                    print(f"control {ctl:2d} value {vh << 8 | vl:5d} page {page} shift {shift}")
        finally:
            m.cmd(0x10, b"\x00")


if __name__ == "__main__":
    main()
