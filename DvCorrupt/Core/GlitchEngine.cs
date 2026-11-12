namespace DvCorrupt.Core;

/// <summary>
/// Applies deck-style error concealment to BGR24 frames. The deck's frame memory is the
/// previous *output* frame, so a macroblock that keeps failing stays frozen on old picture
/// ("delayed" video) until it reads cleanly again.
/// </summary>
public sealed class GlitchEngine
{
    public readonly int W, H, Stride;
    public readonly DvLayout Layout;
    public readonly FrameErrors Errors;
    public readonly byte[] Output;   // clean result (what gets exported)
    public byte[] Display;           // Output, or Output + error-map overlay
    // Output history, one slot per field phase: memory for frame f is the output of f - TapeStep.
    readonly byte[][] hist;
    readonly long[] histFrame;
    readonly byte[] overlay;
    readonly double tapeFps;
    /// <summary>Output frames per tape frame: 1 for 29.97/25 sources, 2 for 59.94p/50p (one frame per field).</summary>
    public readonly int TapeStep;
    public long LastFrame { get; private set; } = long.MinValue;

    public GlitchEngine(DvLayout layout, double fps, int tapeStep = 1)
    {
        Layout = layout;
        W = layout.Width; H = layout.Height; Stride = W * 3;
        TapeStep = Math.Max(1, tapeStep);
        tapeFps = fps / TapeStep;
        Errors = new FrameErrors(layout.Tracks);
        Output = new byte[Stride * H];
        hist = new byte[TapeStep][];
        histFrame = new long[TapeStep];
        for (int i = 0; i < TapeStep; i++) hist[i] = new byte[Stride * H];
        overlay = new byte[Stride * H];
        Display = Output;
        Reset();
    }

    public void Reset()
    {
        Array.Fill(histFrame, long.MinValue);
        LastFrame = long.MinValue;
    }

    public long TapeFrame(long f) => f / TapeStep;

    public void Process(byte[] src, byte[]? next, long f, GlitchParams p)
    {
        // Both fields of a tape frame share one error map (pure function of the tape frame).
        long tf = TapeFrame(f);
        TapeModel.Compute(tf, p, Layout, tapeFps, Errors);
        Buffer.BlockCopy(src, 0, Output, 0, Output.Length);
        int slot = (int)(f % TapeStep);
        byte[] memory;
        if (histFrame[slot] == f - TapeStep) memory = hist[slot];
        else if (LastFrame == f - 1) memory = hist[(int)((f - 1) % TapeStep)];
        else memory = src;
        f = tf;   // per-block randomness below is keyed to the tape frame too

        var rects = Layout.Rects;
        var video = Errors.Video;
        for (int i = 0; i < video.Length; i++)
        {
            if (!video[i]) continue;
            var r = rects[i];
            if (r.W <= 0 || r.H <= 0) continue;
            int mode = p.Mode;
            if (mode == 5)
            {
                uint hsh = Rng.Hash(p.Seed, f, i, 900) % 100;
                mode = hsh < 86 ? 0 : hsh < 95 ? 3 : hsh < 98 ? 4 : 2;
            }
            switch (mode)
            {
                case 0: CopyRect(memory, Output, r); break;
                case 1: CopyRect(next ?? memory, Output, r); break;
                case 2: FillRect(Output, r, 128, 128, 128); break;
                case 3: DcOnly(src, Output, r); break;
                case 4: PinkGreen(src, Output, r, Rng.Hash(p.Seed, f, i, 901)); break;
            }
        }

        Buffer.BlockCopy(Output, 0, hist[slot], 0, Output.Length);
        long outFrame = tf * TapeStep + slot;
        histFrame[slot] = outFrame;
        LastFrame = outFrame;

        if (p.ShowMap)
        {
            Buffer.BlockCopy(Output, 0, overlay, 0, Output.Length);
            for (int i = 0; i < video.Length; i++)
                if (video[i]) Tint(overlay, rects[i]);
            Display = overlay;
        }
        else Display = Output;
    }

    void CopyRect(byte[] s, byte[] d, RectI r)
    {
        int rowBytes = r.W * 3;
        for (int y = r.Y; y < r.Y + r.H; y++)
        {
            int o = y * Stride + r.X * 3;
            Buffer.BlockCopy(s, o, d, o, rowBytes);
        }
    }

    void FillRect(byte[] d, RectI r, byte b, byte g, byte rr)
    {
        for (int y = r.Y; y < r.Y + r.H; y++)
        {
            int o = y * Stride + r.X * 3;
            for (int x = 0; x < r.W; x++, o += 3) { d[o] = b; d[o + 1] = g; d[o + 2] = rr; }
        }
    }

    // DC-only reconstruction: AC coefficients lost, every 8x8 DCT block becomes flat.
    void DcOnly(byte[] s, byte[] d, RectI r)
    {
        int bw = Math.Max(1, (int)Math.Round(8.0 * W / 720)), bh = Math.Max(1, (int)Math.Round(8.0 * H / (Layout.Pal ? 576 : 480)));
        for (int by = r.Y; by < r.Y + r.H; by += bh)
        for (int bx = r.X; bx < r.X + r.W; bx += bw)
        {
            int ey = Math.Min(by + bh, r.Y + r.H), ex = Math.Min(bx + bw, r.X + r.W);
            long sb = 0, sg = 0, sr = 0; int n = 0;
            for (int y = by; y < ey; y++)
            {
                int o = y * Stride + bx * 3;
                for (int x = bx; x < ex; x++, o += 3) { sb += s[o]; sg += s[o + 1]; sr += s[o + 2]; n++; }
            }
            if (n == 0) continue;
            byte b = (byte)(sb / n), g = (byte)(sg / n), rr = (byte)(sr / n);
            for (int y = by; y < ey; y++)
            {
                int o = y * Stride + bx * 3;
                for (int x = bx; x < ex; x++, o += 3) { d[o] = b; d[o + 1] = g; d[o + 2] = rr; }
            }
        }
    }

    // What a decoder shows when it decodes garbage: saturated magenta / green blocks with DCT stripes.
    void PinkGreen(byte[] s, byte[] d, RectI r, uint h)
    {
        int bw = Math.Max(1, (int)Math.Round(8.0 * W / 720)), bh = Math.Max(1, (int)Math.Round(8.0 * H / (Layout.Pal ? 576 : 480)));
        int sub = 0;
        for (int by = r.Y; by < r.Y + r.H; by += bh)
        for (int bx = r.X; bx < r.X + r.W; bx += bw, sub++)
        {
            uint k = Rng.Mix(h + (uint)sub * 0x9E37U);
            bool pink = (k & 1) == 0 ^ ((h >> 7) & 1) == 0 && (k & 6) != 0;
            int fx = (int)((k >> 3) & 7), fy = (int)((k >> 6) & 7);
            float dc = 0.35f + ((k >> 9) & 255) / 255f * 0.6f;
            int ey = Math.Min(by + bh, r.Y + r.H), ex = Math.Min(bx + bw, r.X + r.W);
            for (int y = by; y < ey; y++)
            {
                float cy = MathF.Cos(MathF.PI * fy * ((y - by) + 0.5f) / bh);
                int o = y * Stride + bx * 3;
                for (int x = bx; x < ex; x++, o += 3)
                {
                    float cx = MathF.Cos(MathF.PI * fx * ((x - bx) + 0.5f) / bw);
                    float luma = (s[o] * 29 + s[o + 1] * 150 + s[o + 2] * 77) / (255f * 256f);
                    float v = Math.Clamp(dc + 0.35f * cx * cy, 0, 1) * (0.55f + 0.45f * luma);
                    if (pink) { d[o] = (byte)(230 * v); d[o + 1] = (byte)(50 * v); d[o + 2] = (byte)(245 * v); }
                    else { d[o] = (byte)(50 * v); d[o + 1] = (byte)(235 * v); d[o + 2] = (byte)(40 * v); }
                }
            }
        }
    }

    void Tint(byte[] d, RectI r)
    {
        for (int y = r.Y; y < r.Y + r.H; y++)
        {
            int o = y * Stride + r.X * 3;
            for (int x = 0; x < r.W; x++, o += 3)
            {
                d[o] = (byte)(d[o] * 0.45f);
                d[o + 1] = (byte)(d[o + 1] * 0.45f);
                d[o + 2] = (byte)(d[o + 2] * 0.45f + 140);
            }
        }
    }
}
