# SMC-Mixer editor

A static web page that edits the mixer's configuration over USB-MIDI (Web MIDI, with SysEx):
DAW / CC mode, mappings per mode, page and Shift layer, CC mode's profile and device templates,
global settings, a live monitor, JSON
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

- Pick a **mode** (*DAW* / *CC*: each has its own mappings), a **page** (or *All pages*) and a
  **layer** (*Normal* / *Shift held*), click a control on the drawing, set what it sends. Orange =
  mapped on this page and layer, blue = inherited from a broader one, grey = what the mode does by
  itself (DAW: the stock Mackie layout; CC: the profile). The firmware resolves page + Shift, page,
  all pages + Shift, all pages, then the mode.
- **Settings**: the mixer's mode (also Shift + Left / Right on the mixer), CC mode's profile,
  **Choose a template…** and **Save as template…** ([templates/README.md](templates/README.md)).
  - **Focus groups**: one row of 8 boxes for Rec, one for Select, one box per encoder. Rec on a
    strip makes each encoder with a CC in its box send that CC on the strip's track; empty boxes
    leave the encoder alone. If both rows use the same encoder, only one group can be on at a time
    (on the mixer, both buttons blink and the other group goes off).
  - **Track of strip 1, per page**: strip 1 of each page is that track (= MIDI channel), strips 2-8
    the next ones. To send one control somewhere else, map it.
- **Factory reset**: every mapping and setting back to the defaults (DAW mode, the "OP-XY - blen2r"
  profile, 2 pages), stored at once. Remembered fader / encoder values are kept. Export first.
- **Copy to all pages** moves a mapping to the all-pages layer (e.g. a Shift + fader macro that
  works on every page).
- **Save to mixer** sends the whole configuration; the mixer checks it (CRC) before storing it.
- **Monitor**: every MIDI message the mixer sends; with *highlight controls*, the drawing lights
  up the control you touch and moves the fader caps.

Files: `config.js` (format, encode / decode, resolution, templates, the modes' descriptions; also loads in
Node for tests), `device.js` (Web MIDI and the simulated mixer), `app.js` (the page).

## QA

See [QA.md](../QA.md): `host/selftest.py` offline, `host/mixer_qa.py` on the hardware (the editor
round trip and 45 guided checks).
