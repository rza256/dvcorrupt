# dvcorrupt

Glitch tools that make video look like it played through a failing MiniDV/DV deck: clogged heads,
tape-guide misalignment, tracking drift, dropouts and scratches. These faults become lost DIF blocks,
and the deck conceals them using the real IEC 61834 macroblock shuffle.

* [`DvCorrupt/`](DvCorrupt/README.md): standalone WPF app (.NET 9 + ffmpeg) with real-time preview
  and export for MiniDV `.avi`, P2 HVX200 `.mxf` (cropped to 4:3) and deinterlaced `.mp4`.
* [`DvCorruptOFX/`](DvCorruptOFX/README.md): the same engine as an OpenFX video effect for VEGAS Pro.

The OpenFX headers are a git submodule. Clone with `git clone --recursive`, or run
`git submodule update --init` after cloning.
