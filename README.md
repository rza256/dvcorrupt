# dvcorrupt

Glitch tools that make video look like it played through a failing MiniDV/DV deck: clogged heads,
tape-guide misalignment, tracking drift, dropouts and scratches. These faults become lost DIF blocks,
and the deck conceals them using the real IEC 61834 macroblock shuffle.

* [`DvCorrupt/`](DvCorrupt/README.md): standalone WPF app (.NET 9 + ffmpeg) with real-time preview
  and export for MiniDV `.avi`, P2 HVX200 `.mxf` (cropped to 4:3) and deinterlaced `.mp4`.
* [`DvCorruptOFX/`](DvCorruptOFX/README.md): the same engine as an OpenFX video effect for VEGAS Pro.

The OpenFX headers are a git submodule. Clone with `git clone --recursive`, or run
`git submodule update --init` after cloning.

## Disclaimer

This project is basically 100% vibecoded. The code was written by an AI (Claude) from prompts, and
nobody has reviewed it line by line. The parts that aren't vibecoded are the references below, which
the DV behaviour (tape model, concealment, block shuffle) is based on:

* [DV Video Error Concealment](https://mediaarea.net/DVAnalyzer/dv-video-error-concealment) (MediaArea DVAnalyzer)
* [The challenges and rewards of preserving video from MiniDV tapes](https://theatrecollection.blogs.bristol.ac.uk/2024/05/09/the-challenges-and-rewards-of-preserving-video-from-minidv-tapes/) (University of Bristol Theatre Collection)
* [Re-Winding Compressor](https://michaelchernoff.com/Re-Winding-Compressor) (Michael Chernoff)
* [dvrescue glossary](https://mipops.github.io/dvrescue/sections/glossary.html)
* Sony *DV Mechanical Adjustment Manual VII (Z Mechanism)*, included as
  [`DV_MECHANICAL_ADJUSTMENT_MANUAL_VII(Z_MECHANISM).pdf`](DV_MECHANICAL_ADJUSTMENT_MANUAL_VII(Z_MECHANISM).pdf).
  It is the source of the RF envelope, the guide-misalignment model and the track layout.
* The DV macroblock shuffle tables follow FFmpeg's `dvdata`/`dv_calc_mb_coordinates`.

Use at your own risk.

## License

[WTFPL](LICENSE): do what the fuck you want. This covers the project's own code only. The OpenFX
headers (BSD-3) and the Sony manual keep their own terms.
