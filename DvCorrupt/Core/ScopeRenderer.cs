namespace DvCorrupt.Core;

/// <summary>
/// Draws the per-track RF envelope like the service-manual oscilloscope check:
/// one lane per head sweep (3.3 ms on NTSC), entrance side on the left, exit side on the right.
/// </summary>
public sealed class ScopeRenderer
{
    public const int W = 1200, H = 110;
    public readonly byte[] Buf = new byte[W * H * 3];

    void Px(int x, int y, byte b, byte g, byte r)
    {
        if ((uint)x >= W || (uint)y >= H) return;
        int o = (y * W + x) * 3;
        Buf[o] = b; Buf[o + 1] = g; Buf[o + 2] = r;
    }

    public void Render(FrameErrors e)
    {
        int T = e.Tracks;
        int mid = (H - 10) / 2;
        int amp = mid - 4;
        int thrPx = (int)(e.Threshold * amp);

        for (int i = 0; i < Buf.Length; i += 3) { Buf[i] = 20; Buf[i + 1] = 18; Buf[i + 2] = 17; }

        for (int t = 0; t < T; t++)
        {
            int x0 = t * W / T, x1 = (t + 1) * W / T;
            int lw = x1 - x0;
            bool headA = (t & 1) == 0;
            for (int x = x0; x < x1; x++)
            {
                float p = (x - x0 + 0.5f) / lw;
                // sector shading
                bool aud = p >= TapeModel.AudioStart && p < TapeModel.AudioEnd;
                bool vid = p >= TapeModel.VideoStart && p < TapeModel.VideoEnd;
                byte sb = 20, sg = 18, sr = 17;
                if (aud) { sb = 42; sg = 26; sr = 34; }
                else if (vid) { sb = 28; sg = 27; sr = 24; }
                for (int y = 0; y < H - 10; y++) Px(x, y, sb, sg, sr);

                float fi = p * FrameErrors.EnvRes - 0.5f;
                int i0 = Math.Clamp((int)MathF.Floor(fi), 0, FrameErrors.EnvRes - 1);
                int i1 = Math.Min(i0 + 1, FrameErrors.EnvRes - 1);
                float fr = Math.Clamp(fi - i0, 0, 1);
                float env = e.Env[t * FrameErrors.EnvRes + i0] * (1 - fr) + e.Env[t * FrameErrors.EnvRes + i1] * fr;
                int half = (int)(Math.Min(env, 1.2f) * amp);
                bool below = env < e.Threshold;
                for (int y = mid - half; y <= mid + half; y++)
                {
                    float k = 1 - Math.Abs(y - mid) / (float)Math.Max(1, half) * 0.5f;
                    if (below) Px(x, y, (byte)(60 * k), (byte)(70 * k), (byte)(235 * k));
                    else if (headA) Px(x, y, (byte)(235 * k), (byte)(200 * k), (byte)(70 * k));
                    else Px(x, y, (byte)(70 * k), (byte)(160 * k), (byte)(245 * k));
                }
                if ((x & 3) < 2)
                {
                    Px(x, mid - thrPx, 60, 220, 240);
                    Px(x, mid + thrPx, 60, 220, 240);
                }
            }
            for (int y = 0; y < H; y++) Px(x0, y, 70, 70, 70);

            // error ticks: video (red) and audio (magenta) blocks that failed
            int vb = t * DvLayout.VideoBlocks;
            for (int v = 0; v < DvLayout.VideoBlocks; v++)
            {
                if (!e.Video[vb + v]) continue;
                int x = x0 + (int)(e.VideoPos[vb + v] * lw);
                for (int y = H - 8; y < H - 1; y++) Px(x, y, 50, 50, 255);
            }
            for (int b = 0; b < FrameErrors.AudioBlocks; b++)
            {
                if (!e.Audio[t * FrameErrors.AudioBlocks + b]) continue;
                int x = x0 + (int)(TapeModel.AudioBlockPos(b) * lw);
                for (int y = H - 8; y < H - 1; y++) { Px(x, y, 255, 60, 230); Px(x + 1, y, 255, 60, 230); }
            }
        }
    }
}
