# Templates

A template sets the mixer's **CC mode** for one device: the profile (which CC the faders, encoders,
Mute, Solo and focus buttons send), the pages and their tracks, and any extra mappings. DAW mode
and personal preferences (fader takeover, LEDs, encoder acceleration) are not part of it. In the
editor: Settings → **Choose a template…** (search, ★ to keep favourites on top).

Each template is two files:

- `<name>.json`: what the editor loads;
- `<name>.md`: how it is mapped, for people deciding whether to use it. **Required.**

## Naming

A template's name says whose it is, because two people's setups for the same device differ:
`OP-XY - blen2r`, `OP-XY - adam`, `OP-XY - blen2r alt 1`. It must contain ` - <author>` and fit in
28 ASCII characters (the mixer stores it). File names are the name in lower case with dashes:
`opxy-blen2r.json`, `opxy-blen2r.md`.

## Making one

1. In the editor, set CC mode up for your device: Settings → CC mode profile, pages, tracks, and
   mappings with the Mode bar on **CC**. Try it on the device.
2. Settings → **Save as template…**: give your name, the device and a one-line description. The
   editor downloads `<name>.json` and a `<name>.md` started from the mapping.
3. Finish the `.md`: what the device needs (its MIDI settings, channels), what each control does
   that is not obvious, what does not work.
4. Put both files in `editor/templates/local/` to keep them to yourself (git ignores that folder),
   or in `editor/templates/` to share them. Then run:

       python3 host/templates_index.py

   It checks every template and writes the `index.json` lists the editor reads. Reload the editor.

## Sharing one

Open a pull request with the two files and the updated `editor/templates/index.json`. Pull requests
for new devices are welcome; this repository is not maintained, so they are reviewed when there is
time, maybe never. Forking and keeping your own list is just as fine.
