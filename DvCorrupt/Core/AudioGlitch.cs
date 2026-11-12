namespace DvCorrupt.Core;

/// <summary>
/// DV audio errors. 48 kHz stereo: CH1 lives in the first half of a frame's tracks, CH2 in the
/// second half; samples are shuffled across tracks and audio DIF blocks. The deck interpolates
/// over isolated lost samples and mutes when too much is lost (giving clicks / drop-outs).
/// </summary>
public static class AudioGlitch
{
    public const int Rate = 48000;

    public static long SampleAt(long frame, int fpsNum, int fpsDen)
        => frame * Rate * fpsDen / fpsNum;

    /// <param name="pcm">interleaved stereo s16</param>
    /// <param name="pcmStart">absolute sample index of pcm[0]</param>
    public static void ApplyFrame(short[] pcm, long pcmStart, long frame, int fpsNum, int fpsDen, FrameErrors e)
    {
        if (e.AudioErrorCount == 0) return;
        long s0 = SampleAt(frame, fpsNum, fpsDen) - pcmStart;
        long s1 = SampleAt(frame + 1, fpsNum, fpsDen) - pcmStart;
        long total = pcm.Length / 2;
        s0 = Math.Clamp(s0, 0, total); s1 = Math.Clamp(s1, 0, total);
        int n = (int)(s1 - s0);
        if (n <= 0) return;

        int half = Math.Max(1, e.Tracks / 2);
        var bad = new bool[n];
        for (int ch = 0; ch < 2; ch++)
        {
            int badCount = 0;
            for (int i = 0; i < n; i++)
            {
                int tr = ch * half + (i / 3 + 2 * (i % 3)) % half;
                int blk = (3 * (i % 3) + (i % 45) / 15) % FrameErrors.AudioBlocks;
                bad[i] = e.Audio[tr * FrameErrors.AudioBlocks + blk];
                if (bad[i]) badCount++;
            }
            if (badCount == 0) continue;

            if (badCount > n * 0.3)
            {
                for (int i = 0; i < n; i++) pcm[(s0 + i) * 2 + ch] = 0;   // deck mute
                continue;
            }

            // linear interpolation across bad runs
            int k = 0;
            while (k < n)
            {
                if (!bad[k]) { k++; continue; }
                int a = k - 1;
                int b = k;
                while (b < n && bad[b]) b++;
                short va = a >= 0 ? pcm[(s0 + a) * 2 + ch] : (short)0;
                short vb = b < n ? pcm[(s0 + b) * 2 + ch] : va;
                int span = b - a;
                for (int i = k; i < b; i++)
                {
                    float t = (i - a) / (float)span;
                    pcm[(s0 + i) * 2 + ch] = (short)(va + (vb - va) * t);
                }
                k = b;
            }
        }
    }
}
