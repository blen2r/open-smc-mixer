# SMC-Mixer editor

A static web page that edits the mixer's configuration over USB-MIDI (Web MIDI, with SysEx):
mappings per page and Shift layer, the base profile and global settings, a live monitor, JSON
import / export, and a simulated mixer for working without the hardware. The model and the
protocol are in [docs/mixer-config.md](../docs/mixer-config.md); the firmware side is
`fw/smcpad/examples/mixer/mixer_config.c`.

## Run it

Web MIDI needs Chrome, Edge or Chromium and a secure origin (localhost is fine):

    host/mixer_editor.sh            # serves editor/ on http://localhost:8770 and opens Chrome

or `python3 -m http.server -d editor 8770` and open <http://localhost:8770>. Plug the mixer in
over USB (our firmware, "SMC-Mixer open fw"), press **Connect (USB)** and allow MIDI / SysEx
access. **Simulate** (or `?sim` in the URL) works without the hardware; its configuration is kept
in the browser.

## Use it

- Pick a **page** (or *All pages*) and a **layer** (*Normal* / *Shift held*), click a control on
  the drawing, set what it sends. Orange = mapped on this page and layer, blue = inherited from a
  broader one, grey = what the base profile does. The firmware resolves page + Shift, page, all
  pages + Shift, all pages, then the base profile.
- **Copy to all pages** moves a mapping to the all-pages layer (e.g. a Shift + fader macro that
  works on every page).
- **Save to mixer** sends the whole configuration; the mixer checks it (CRC) before storing it.
- **Monitor**: every MIDI message the mixer sends; with *highlight controls*, the drawing lights
  up the control you touch and moves the fader caps.

Files: `config.js` (format, encode / decode, resolution, base profile descriptions; also loads in
Node for tests), `device.js` (Web MIDI and the simulated mixer), `app.js` (the page).

## QA

`host/mixer_qa.py` (Python with `mido`) checks the editor and the firmware together: you import
`host/qa/qa_config.json` in the editor, make four listed changes and save; the script reads the
configuration back and compares it, then walks you through 28 hardware checks ("turn knob 1 right")
and verifies the MIDI the mixer sends on USB. Your configuration is backed up first and restored at
the end. `--no-editor` skips the editor part, `--from N` resumes at a check.
