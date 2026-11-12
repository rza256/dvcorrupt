using System.Diagnostics;
using System.Runtime.InteropServices;
using DvCorrupt.Core;
using DvCorrupt.Media;

namespace DvCorrupt;

/// <summary>
/// Background playback: ffmpeg decodes into a ring of source frames, the engine conceals,
/// the UI is notified. When paused, any parameter change re-simulates the last ~24 frames
/// so the frozen-block "memory" of the deck is correct for the still frame you're looking at.
/// </summary>
public sealed class Player : IDisposable
{
    int Preroll = 24;   // output frames re-simulated before a still frame (scaled by tape step)
    int RingSize = 48;
    int step = 1;       // output frames per tape frame

    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint p);
    [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint p);

    readonly object cmdLock = new();
    readonly AutoResetEvent wake = new(false);
    readonly Thread thread;
    volatile bool quit;

    SourceSpec? pendingSource;
    long? pendingSeek;
    bool? pendingPlay;
    bool pendingRerender;
    volatile GlitchParams prm = new();

    // worker-only state
    SourceSpec? spec;
    FfmpegProcess? reader;
    long readerNext, readerStart;
    bool readerEof;
    byte[][] ring = [];
    long[] ringIdx = [];
    GlitchEngine? engine;
    readonly ScopeRenderer scope = new();
    long cur;
    bool playing;

    // presentation (guarded by PresentLock)
    public readonly object PresentLock = new();
    public byte[] PresentFrame = [];
    public int PresentW, PresentH;
    public readonly byte[] PresentScope = new byte[ScopeRenderer.W * ScopeRenderer.H * 3];
    public long PresentIndex;
    public int PresentVideoErrors, PresentAudioErrors;
    public float PresentGainA, PresentGainB;

    public event Action? Presented;
    public event Action<bool>? PlayStateChanged;
    public event Action<string>? Error;

    public Player()
    {
        timeBeginPeriod(1);
        thread = new Thread(Run) { IsBackground = true, Name = "Player", Priority = ThreadPriority.AboveNormal };
        thread.Start();
    }

    public void Open(SourceSpec s, long frame)
    {
        lock (cmdLock) { pendingSource = s; pendingSeek = frame; }
        wake.Set();
    }

    public void Seek(long frame)
    {
        lock (cmdLock) pendingSeek = frame;
        wake.Set();
    }

    public void SetPlaying(bool play)
    {
        lock (cmdLock) pendingPlay = play;
        wake.Set();
    }

    public void SetParams(GlitchParams p)
    {
        prm = p;
        lock (cmdLock) pendingRerender = true;
        wake.Set();
    }

    long Clamp(long f) => spec == null ? 0 : Math.Clamp(f, 0, Math.Max(0, spec.FrameCount - 1));

    void Run()
    {
        var sw = Stopwatch.StartNew();
        double clockT0 = 0;
        long clockF0 = 0;
        while (!quit)
        {
            SourceSpec? ns; long? sk; bool? pl; bool rr;
            lock (cmdLock)
            {
                ns = pendingSource; pendingSource = null;
                sk = pendingSeek; pendingSeek = null;
                pl = pendingPlay; pendingPlay = null;
                rr = pendingRerender; pendingRerender = false;
            }
            try
            {
                if (ns != null) { SetupSource(ns); sk ??= cur; }
                if (spec == null || engine == null) { wake.WaitOne(100); continue; }

                if (sk.HasValue)
                {
                    DoSeek(Clamp(sk.Value));
                    rr = false;
                    clockT0 = sw.Elapsed.TotalSeconds; clockF0 = cur;
                }
                if (pl.HasValue && pl.Value != playing)
                {
                    playing = pl.Value;
                    if (playing && cur >= spec.FrameCount - 1) DoSeek(0);
                    clockT0 = sw.Elapsed.TotalSeconds; clockF0 = cur;
                    PlayStateChanged?.Invoke(playing);
                }

                if (playing)
                {
                    double target = clockT0 + (cur + 1 - clockF0) / spec.Fps;
                    double now = sw.Elapsed.TotalSeconds;
                    if (target - now > 0.002)
                    {
                        wake.WaitOne(TimeSpan.FromSeconds(target - now));
                        continue;
                    }
                    long next = cur + 1;
                    FillTo(next + step);
                    if (Get(next) == null)
                    {
                        playing = false;
                        PlayStateChanged?.Invoke(false);
                        continue;
                    }
                    Render(next, false);
                    cur = next;
                    Present();
                    if (now - target > 0.25) { clockT0 = now; clockF0 = cur; }
                }
                else
                {
                    if (rr) { Render(cur, true); Present(); }
                    wake.WaitOne(250);
                }
            }
            catch (Exception ex)
            {
                if (playing) { playing = false; PlayStateChanged?.Invoke(false); }
                Error?.Invoke(ex.Message);
                wake.WaitOne(250);
            }
        }
    }

    void SetupSource(SourceSpec s)
    {
        reader?.Dispose();
        reader = null;
        spec = s;
        step = s.TapeStep;
        Preroll = 24 * step;
        RingSize = Preroll + 8 * step + 8;
        ring = new byte[RingSize][];
        for (int i = 0; i < RingSize; i++) ring[i] = new byte[s.FrameBytes];
        ringIdx = Enumerable.Repeat(-1L, RingSize).ToArray();
        engine = new GlitchEngine(new DvLayout(s.Info.IsPal, s.OutW, s.OutH), s.Fps, step);
        readerEof = true;
        readerNext = 0;
    }

    byte[]? Get(long f)
    {
        if (f < 0) return null;
        int slot = (int)(f % RingSize);
        return ringIdx[slot] == f ? ring[slot] : null;
    }

    void OpenReaderAt(long start)
    {
        reader?.Dispose();
        Array.Fill(ringIdx, -1L);
        reader = new FfmpegProcess(spec!.ReaderArgs(start), readStdout: true, writeStdin: false);
        readerNext = start;
        readerStart = start;
        readerEof = false;
    }

    void FillTo(long target)
    {
        while (!readerEof && reader != null && readerNext <= target)
        {
            int slot = (int)(readerNext % RingSize);
            ringIdx[slot] = -1;
            if (!reader.ReadExact(ring[slot], spec!.FrameBytes))
            {
                readerEof = true;
                if (readerNext == readerStart)
                {
                    Thread.Sleep(80);   // let stderr arrive
                    string err = reader.ErrorText;
                    throw new Exception(err.Length > 0 ? "ffmpeg: " + err : "ffmpeg produced no frames");
                }
                break;
            }
            ringIdx[slot] = readerNext;
            readerNext++;
        }
    }

    void DoSeek(long f)
    {
        bool reuse = reader != null && Get(f) != null
                     || reader != null && !readerEof && f >= readerNext && f <= readerNext + 90;
        if (!reuse) OpenReaderAt(Math.Max(0, f - Preroll));
        FillTo(f + step);
        if (Get(f) == null)
        {
            // past the real end of the stream: clamp to the last frame we have
            long last = readerNext - 1;
            if (last < 0) return;
            f = last;
        }
        Render(f, false);
        cur = f;
        Present();
    }

    void Render(long f, bool forceWarm)
    {
        var e = engine!;
        var p = prm;
        var src = Get(f);
        if (src == null) return;
        if (!forceWarm && e.LastFrame == f - 1)
        {
            e.Process(src, Get(f + step), f, p);
            return;
        }
        long start = f;
        while (start > 0 && f - start < Preroll && Get(start - 1) != null) start--;
        e.Reset();
        for (long g = start; g <= f; g++)
            e.Process(Get(g)!, Get(g + step), g, p);
    }

    void Present()
    {
        var e = engine!;
        scope.Render(e.Errors);
        lock (PresentLock)
        {
            if (PresentFrame.Length != e.Display.Length) PresentFrame = new byte[e.Display.Length];
            Buffer.BlockCopy(e.Display, 0, PresentFrame, 0, e.Display.Length);
            Buffer.BlockCopy(scope.Buf, 0, PresentScope, 0, PresentScope.Length);
            PresentW = e.W; PresentH = e.H;
            PresentIndex = cur;
            PresentVideoErrors = e.Errors.VideoErrorCount;
            PresentAudioErrors = e.Errors.AudioErrorCount;
            PresentGainA = e.Errors.HeadGain[0];
            PresentGainB = e.Errors.HeadGain[1];
        }
        Presented?.Invoke();
    }

    public void Dispose()
    {
        quit = true;
        wake.Set();
        thread.Join(1000);
        reader?.Dispose();
        timeEndPeriod(1);
    }
}
