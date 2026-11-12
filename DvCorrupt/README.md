# DV Corrupt

Real-time preview + export of DV deck error-concealment glitches for MiniDV `.avi`, P2 (HVX200) `.mxf`,
and deinterlaced double-rate `.mp4` (e.g. HVX200 1080i bwdif'd to 59.94p, H.265).
Needs `ffmpeg`/`ffprobe` on PATH (or next to the exe) and the .NET 9 desktop runtime.

    dotnet run -c Release                 # or bin\Release\net9.0-windows\DvCorrupt.exe [file]

## How it works
* **Tape model** (`Core/TapeModel.cs`): every frame is 10 helical tracks (NTSC; 12 PAL), alternately
  read by head A / head B. For each track an RF envelope is computed (head clog gain, TG3 entrance /
  TG5 exit guide error, tracking offset + tension wobble, dropouts spanning adjacent tracks,
  lengthwise scratch, fluctuation), the same picture as the service-manual scope check. Blocks whose
  envelope falls under the read threshold fail (soft "digital cliff"); a track with only a few bad
  blocks is fixed by the outer (C2) code.
* **Layout** (`Core/DvLayout.cs`): failed DIF blocks are mapped to screen macroblocks with the real
  IEC 61834 shuffle, so a dead head gives horizontal bands, guide errors fill each super block from
  its top-left, and scratches give a regular grid. Sweep mappings put track position straight on the
  frame diagonal / raster instead.
* **Concealment** (`Core/GlitchEngine.cs`): copy from the deck's previous *output* frame (type 2;
  repeated failures freeze), next frame (4), grey (F), DC-only blocks, pink/green garbage, or mixed.
* **Audio** (export only): audio sits at the entrance end of each track; lost samples are
  interpolated, and the channel is muted when too much is lost.

Wide MXF (1280x1080, 3:2 pixels) is cropped to a 4:3 window (pan with *4:3 crop position*) and scaled
to the working size. Keys: Space play/pause, ←/→ frame step (Shift = 10), I / O set export range.

Double-rate progressive sources (59.94p / 50p) are treated as one frame per field: each pair of
frames shares one DV tape frame's error map, and concealment copies the same field of the previous
tape frame. On export they can be woven back to 29.97i / 25i (always done for DV, which is BFF).
