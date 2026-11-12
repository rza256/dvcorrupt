namespace DvCorrupt.Core;

public sealed class FrameErrors
{
    public const int EnvRes = 96;
    public const int AudioBlocks = 9;

    public readonly int Tracks;
    public readonly bool[] Video;      // [t*135+v]
    public readonly float[] VideoPos;  // track position used for each block (for the scope)
    public readonly bool[] Audio;      // [t*9+b]
    public readonly float[] Env;       // [t*EnvRes+i] RF envelope for the scope
    public readonly float[] HeadGain = new float[2];
    public int VideoErrorCount, AudioErrorCount;
    public float Threshold;

    internal readonly Dropout[] Drops = new Dropout[64];
    internal int DropCount;

    public FrameErrors(int tracks)
    {
        Tracks = tracks;
        Video = new bool[tracks * DvLayout.VideoBlocks];
        VideoPos = new float[tracks * DvLayout.VideoBlocks];
        Audio = new bool[tracks * AudioBlocks];
        Env = new float[tracks * EnvRes];
    }

    internal struct Dropout { public int T0, Span; public float Pos, Len, Depth, Slant; }
}

/// <summary>
/// Simulates the playback RF envelope of every helical track and decides which DIF blocks
/// fail error correction. Modelled on the deck service-manual tape path check: the envelope
/// of each head sweep should be flat; a misaligned entrance guide (TG3) drops the start of
/// each sweep, a misaligned exit guide (TG5) drops the end, a clogged head drops its whole
/// sweep, and the error-correction "cliff" turns a weak envelope into lost blocks.
///
/// Everything is a pure function of (frame index, params) so frames can be re-rendered.
/// </summary>
public static class TapeModel
{
    // Approximate sector layout along one track (ITI, audio, video, subcode).
    public const float AudioStart = 0.04f, AudioEnd = 0.14f;
    public const float VideoStart = 0.16f, VideoEnd = 0.94f;

    // video sector = 149 sync blocks: 2 VAUX, 135 video, 1 VAUX, 11 C2 parity
    public static float VideoBlockPos(int v) => VideoStart + (VideoEnd - VideoStart) * (2 + v + 0.5f) / 149f;
    public static float AudioBlockPos(int b) => AudioStart + (AudioEnd - AudioStart) * (b + 0.5f) / FrameErrors.AudioBlocks;

    struct TrackState
    {
        public float Gain, A, E, X;
        public bool Lost;
    }

    public static void Compute(long f, GlitchParams p, DvLayout layout, double fps, FrameErrors e)
    {
        int T = layout.Tracks;
        int seed = p.Seed;
        double time = f / fps;

        // --- tape path: tracking offset and guide errors with slow tension wobble ---
        double ws = p.WobbleSpeed;
        double wn1 = Rng.ValueNoise(seed, time * ws, 11) * 2 - 1;
        double wn2 = Rng.ValueNoise(seed, time * ws * 1.37 + 5.1, 12) * 2 - 1;
        double wn3 = Rng.ValueNoise(seed, time * ws * 0.83 + 9.7, 13) * 2 - 1;
        double jit = (Rng.F(seed, f, 14) * 2 - 1) * p.Jitter;
        float A = (float)Math.Abs(p.TrackingOffset + p.Wobble * 0.6 * wn1 + jit);
        float E = (float)Math.Max(0, p.Entrance * (1 + p.Wobble * wn2) + (p.Entrance > 0 ? jit : 0));
        float X = (float)Math.Max(0, p.Exit * (1 + p.Wobble * wn3) + (p.Exit > 0 ? jit : 0));

        // --- heads ---
        for (int h = 0; h < 2; h++)
        {
            double clog = h == 0 ? p.HeadAClog : p.HeadBClog;
            double flick = 0.5 * Rng.ValueNoise(seed, time * 3, 20 + h) + 0.5 * Rng.F(seed, f, 22 + h);
            double c = clog * (1 + p.ClogFlicker * (2 * flick - 1));
            if (p.DebrisRate > 0)
            {
                int dur = Math.Max(1, (int)p.DebrisDuration);
                long w = (f + h * 7919L) / dur;
                double prob = p.DebrisRate / 60.0 / fps * dur;
                if (Rng.F(seed, w, 30 + h) < prob) c += 0.6 + 0.45 * Rng.F(seed, w, 32 + h);
            }
            e.HeadGain[h] = (float)Math.Clamp(1 - c, 0, 1);
        }

        // --- dropouts (oxide defects can cross several adjacent tracks) ---
        int n = (int)p.DropoutRate;
        if (Rng.F(seed, f, 40) < p.DropoutRate - n) n++;
        n = Math.Min(n, e.Drops.Length);
        e.DropCount = n;
        for (int i = 0; i < n; i++)
        {
            e.Drops[i] = new FrameErrors.Dropout
            {
                T0 = (int)(Rng.F(seed, f, 41, i) * T),
                Pos = Rng.F(seed, f, 42, i),
                Len = (float)(p.DropoutLength * (0.4 + 1.2 * Rng.F(seed, f, 43, i))),
                Span = 1 + (int)(Rng.F(seed, f, 44, i) * 2.6f),
                Depth = (float)(p.DropoutDepth * (0.6 + 0.4 * Rng.F(seed, f, 45, i))),
                Slant = (Rng.F(seed, f, 46, i) - 0.5f) * 0.04f,
            };
        }

        float sPos = (float)(p.ScratchPos + 0.008 * (Rng.ValueNoise(seed, time * 2, 50) * 2 - 1));
        float sHalf = (float)p.ScratchWidth * 0.5f, sDepth = (float)p.ScratchDepth;
        float fluct = (float)p.Fluctuation;
        float thr = (float)p.Threshold, soft = (float)Math.Max(1e-4, p.Softness);
        int capV = (int)Math.Round(p.OuterEcc * 11);
        int capA = (int)Math.Round(p.OuterEcc * 5);
        e.Threshold = thr;
        e.VideoErrorCount = 0;
        e.AudioErrorCount = 0;

        float[] posTab = p.Mapping == 1 ? layout.UDiag : p.Mapping == 2 ? layout.URaster : null!;

        for (int t = 0; t < T; t++)
        {
            var ts = new TrackState
            {
                Gain = e.HeadGain[t & 1],
                A = A, E = E, X = X,
                Lost = Rng.F(seed, f, 60, t) < p.TrackLoss,
            };
            float waveOff = t * 17.3f + (f % 100000) * 3.11f;

            float Env(float pos, bool withBlockNoise, int salt)
            {
                float dev = ts.A + ts.E * MathF.Pow(1 - pos, 1.2f) + ts.X * MathF.Pow(pos, 1.2f);
                float env = ts.Gain * Math.Clamp(1 - dev, 0, 1);
                for (int i = 0; i < e.DropCount; i++)
                {
                    ref var d = ref e.Drops[i];
                    int dt = t - d.T0;
                    if (dt < 0 || dt >= d.Span) continue;
                    float c = d.Pos + d.Slant * dt;
                    float dist = Math.Abs(pos - c);
                    if (dist < d.Len * 0.5f)
                    {
                        float edge = 1 - dist / (d.Len * 0.5f);
                        env *= 1 - d.Depth * Math.Min(1, edge * 3);
                    }
                }
                if (sDepth > 0 && Math.Abs(pos - sPos) < sHalf) env *= 1 - sDepth;
                float wave = Rng.ValueNoise(seed, pos * 6 + waveOff, 75) * 2 - 1;
                float noise = withBlockNoise ? Rng.F(seed, f, salt, 70 + t) * 2 - 1 : 0;
                env *= 1 + fluct * (0.55f * wave + 0.45f * noise);
                return Math.Max(0, env);
            }

            bool Fails(float env, int salt)
            {
                float pr = 1f / (1f + MathF.Exp(-(thr - env) / soft));
                return Rng.F(seed, f, salt, 90 + t) < pr;
            }

            // video sector
            int vBase = t * DvLayout.VideoBlocks, vErr = 0;
            for (int v = 0; v < DvLayout.VideoBlocks; v++)
            {
                float pos = posTab == null
                    ? VideoBlockPos(v)
                    : VideoStart + (VideoEnd - VideoStart) * posTab[vBase + v];
                e.VideoPos[vBase + v] = pos;
                bool bad = ts.Lost || Fails(Env(pos, true, v), v);
                e.Video[vBase + v] = bad;
                if (bad) vErr++;
            }
            if (!ts.Lost && vErr <= capV)
            {
                Array.Clear(e.Video, vBase, DvLayout.VideoBlocks);
                vErr = 0;
            }
            e.VideoErrorCount += vErr;

            // audio sector
            int aBase = t * FrameErrors.AudioBlocks, aErr = 0;
            for (int b = 0; b < FrameErrors.AudioBlocks; b++)
            {
                float pos = AudioBlockPos(b);
                bool bad = ts.Lost || Fails(Env(pos, true, 500 + b), 500 + b);
                e.Audio[aBase + b] = bad;
                if (bad) aErr++;
            }
            if (!ts.Lost && aErr <= capA)
            {
                Array.Clear(e.Audio, aBase, FrameErrors.AudioBlocks);
                aErr = 0;
            }
            e.AudioErrorCount += aErr;

            // envelope trace for the scope
            int eBase = t * FrameErrors.EnvRes;
            for (int i = 0; i < FrameErrors.EnvRes; i++)
            {
                float pos = (i + 0.5f) / FrameErrors.EnvRes;
                e.Env[eBase + i] = ts.Lost ? 0 : Env(pos, false, 0);
            }
        }
    }
}
