# DV Corrupt — OpenFX plugin for VEGAS Pro

The DV Corrupt tape-deck simulator as a native VEGAS Pro video effect (OpenFX), with the same model as
the standalone app in `..\DvCorrupt`: clogged heads, TG3/TG5 guide misalignment, tracking drift,
dropouts, scratches → per-track RF envelope → lost DIF blocks → deck concealment using the real
IEC 61834 macroblock shuffle.

## Install
1. `build.bat` (needs VS 2022 at `D:\VS2022`; produces `build\DvCorrupt.ofx.bundle`)
2. `powershell -ExecutionPolicy Bypass -File install.ps1` (UAC prompt; copies the bundle to
   `C:\Program Files\Common Files\OFX\Plugins`). `install.ps1 -Uninstall` removes it.
3. Restart VEGAS. The effect is under **Video FX → DV Corrupt (Tape Error Concealment)**, with 12
   presets in the preset dropdown (Clogged head A, TG3 entrance misaligned, Guide sweep, Dying deck…).

## Using it in VEGAS
* Put it on the media/event (or track). For HVX200 MXF/MP4 footage, do the 4:3 crop with Pan/Crop
  or the project size first; the effect maps the DV block layout onto whatever frame it receives.
* All sliders animate (keyframes). Choice/seed/checkbox params are static.
* **Deck / format**: DV system (Auto picks PAL for 25/50 fps). 59.94p / 50p projects are treated as
  one frame per field, so frame pairs share one tape frame like the standalone app.
  *Deck memory (seconds)*: a concealed block walks back through the tape's error history to the last
  frame where it read cleanly and shows that picture. A block dead for longer than this refreshes to a
  newer frozen picture once per period. Every frame is a pure function of its frame number, because
  VEGAS renders with ~14 parallel instances, out of order, and twice per frame (t and t+0.5) in
  interlaced projects.
  *Show RF scope* draws the per-track RF envelope across the bottom of the frame.
* Works in 8-bit and 32-bit float projects.
* Audio glitches are not part of the OFX effect (OFX is video-only); use the standalone app's export
  for tape-accurate audio dropouts.

## Files
* `src/DvEngine.h` — engine (tape model, layout, concealment), header-only C++17
* `src/DvCorruptOFX.cpp` — OFX entry points, parameters, deck-memory cache
* `tests/ofxtest.cpp` — minimal OFX host: renders footage through the plugin; also writes the
  preset package (`ofxtest presets out.xml`)
* `tests/VegasRenderTest.cs` — VEGAS script that builds a project, applies the effect + presets and
  renders (run `vegas210.exe -SCRIPT:"…\VegasRenderTest.cs"`)
* `external/openfx` — OpenFX API headers (BSD-3, AcademySoftwareFoundation/openfx)
