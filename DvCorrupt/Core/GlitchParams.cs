using System.Text.Json;

namespace DvCorrupt.Core;

public enum ParamKind { Float, Int, Enum, Bool }

public sealed record ParamDef(
    string Group, string Label, string Tip, ParamKind Kind,
    double Min, double Max, string Fmt,
    Func<GlitchParams, double> Get, Action<GlitchParams, double> Set,
    string[]? Options = null);

public sealed class GlitchParams
{
    // ---- Video heads (drum has two heads, A and B, that alternate tracks) ----
    public double HeadAClog;
    public double HeadBClog;
    public double ClogFlicker = 0.5;
    public double DebrisRate;          // sudden clog events per minute
    public double DebrisDuration = 8;  // frames

    // ---- Tape path / tracking (TG3 = entrance guide, TG5 = exit guide) ----
    public double Entrance;
    public double Exit;
    public double TrackingOffset;
    public double Wobble;
    public double WobbleSpeed = 0.5;   // Hz
    public double Jitter;
    public int Mapping;                // 0 authentic shuffle, 1 diagonal sweep, 2 raster sweep

    // ---- Tape condition ----
    public double Fluctuation = 0.08;
    public double DropoutRate;         // per frame
    public double DropoutLength = 0.03;
    public double DropoutDepth = 0.9;
    public double ScratchDepth;
    public double ScratchPos = 0.55;
    public double ScratchWidth = 0.02;
    public double TrackLoss;

    // ---- Error correction ----
    public double Threshold = 0.45;
    public double Softness = 0.04;
    public double OuterEcc = 0.35;

    // ---- Concealment ----
    public int Mode;                   // see ConcealModes
    public int Seed = 1;
    public bool ShowMap;

    public GlitchParams Clone() => (GlitchParams)MemberwiseClone();

    public static readonly string[] ConcealModes =
    {
        "Previous frame (type 2)",
        "Next frame (type 4)",
        "Grey blocks (type F)",
        "Blocky / DC only",
        "Pink & green (no concealment)",
        "Mixed"
    };

    public static readonly string[] MappingModes =
    {
        "Authentic DV shuffle",
        "Diagonal sweep (TL → BR)",
        "Raster sweep (scanline)"
    };

    static double B(bool b) => b ? 1 : 0;

    public static readonly ParamDef[] Defs =
    {
        new("Video heads", "Head A clog", "Signal loss on head A (even tracks). Full clog = every other band of blocks frozen.",
            ParamKind.Float, 0, 1, "0.00", p => p.HeadAClog, (p, v) => p.HeadAClog = v),
        new("Video heads", "Head B clog", "Signal loss on head B (odd tracks).",
            ParamKind.Float, 0, 1, "0.00", p => p.HeadBClog, (p, v) => p.HeadBClog = v),
        new("Video heads", "Clog flicker", "How intermittently the clog comes and goes frame to frame.",
            ParamKind.Float, 0, 1, "0.00", p => p.ClogFlicker, (p, v) => p.ClogFlicker = v),
        new("Video heads", "Debris hits / min", "Sudden shed-particle clogs that blank a head for a moment.",
            ParamKind.Float, 0, 120, "0", p => p.DebrisRate, (p, v) => p.DebrisRate = v),
        new("Video heads", "Debris duration (frames)", "How long each debris clog lasts.",
            ParamKind.Float, 1, 90, "0", p => p.DebrisDuration, (p, v) => p.DebrisDuration = v),

        new("Tape path / tracking", "Entrance side (TG3)", "Guide misalignment where the head enters the track: start of every track is lost. Concealment grows from the top-left of each super block.",
            ParamKind.Float, 0, 2, "0.00", p => p.Entrance, (p, v) => p.Entrance = v),
        new("Tape path / tracking", "Exit side (TG5)", "Guide misalignment where the head leaves the track: end of every track is lost.",
            ParamKind.Float, 0, 2, "0.00", p => p.Exit, (p, v) => p.Exit = v),
        new("Tape path / tracking", "Tracking offset", "Head rides off the centre of the track along its whole length.",
            ParamKind.Float, 0, 1, "0.00", p => p.TrackingOffset, (p, v) => p.TrackingOffset = v),
        new("Tape path / tracking", "Tension wobble", "Slow drift of tracking / guide error (tape stretch, back-tension).",
            ParamKind.Float, 0, 1, "0.00", p => p.Wobble, (p, v) => p.Wobble = v),
        new("Tape path / tracking", "Wobble speed (Hz)", "",
            ParamKind.Float, 0.05, 6, "0.00", p => p.WobbleSpeed, (p, v) => p.WobbleSpeed = v),
        new("Tape path / tracking", "Frame jitter", "Random per-frame tracking error.",
            ParamKind.Float, 0, 0.5, "0.000", p => p.Jitter, (p, v) => p.Jitter = v),
        new("Tape path / tracking", "Error → screen mapping", "Authentic = real DV macroblock shuffle. Sweeps map track position straight onto the screen so guide errors grow from top-left to bottom-right of the whole frame.",
            ParamKind.Enum, 0, 2, "", p => p.Mapping, (p, v) => p.Mapping = (int)v, MappingModes),

        new("Tape condition", "RF fluctuation", "Envelope amplitude noise (spec allows ~10%).",
            ParamKind.Float, 0, 0.6, "0.00", p => p.Fluctuation, (p, v) => p.Fluctuation = v),
        new("Tape condition", "Dropouts / frame", "Oxide defects; each can span adjacent tracks.",
            ParamKind.Float, 0, 30, "0.0", p => p.DropoutRate, (p, v) => p.DropoutRate = v),
        new("Tape condition", "Dropout length", "Fraction of a track.",
            ParamKind.Float, 0.005, 0.4, "0.000", p => p.DropoutLength, (p, v) => p.DropoutLength = v),
        new("Tape condition", "Dropout depth", "",
            ParamKind.Float, 0, 1, "0.00", p => p.DropoutDepth, (p, v) => p.DropoutDepth = v),
        new("Tape condition", "Scratch depth", "Lengthwise scratch: hits the same spot on every track → regular grid of errors.",
            ParamKind.Float, 0, 1, "0.00", p => p.ScratchDepth, (p, v) => p.ScratchDepth = v),
        new("Tape condition", "Scratch position", "Position across the tape width (= along each track).",
            ParamKind.Float, 0, 1, "0.000", p => p.ScratchPos, (p, v) => p.ScratchPos = v),
        new("Tape condition", "Scratch width", "",
            ParamKind.Float, 0.002, 0.25, "0.000", p => p.ScratchWidth, (p, v) => p.ScratchWidth = v),
        new("Tape condition", "Track sync loss", "Chance a whole track is unreadable.",
            ParamKind.Float, 0, 0.5, "0.000", p => p.TrackLoss, (p, v) => p.TrackLoss = v),

        new("Error correction", "Read threshold", "RF level below which Reed-Solomon can't recover a block (the digital cliff).",
            ParamKind.Float, 0.05, 0.95, "0.00", p => p.Threshold, (p, v) => p.Threshold = v),
        new("Error correction", "Cliff softness", "Low = hard digital cliff; high = errors fade in gradually.",
            ParamKind.Float, 0.005, 0.25, "0.000", p => p.Softness, (p, v) => p.Softness = v),
        new("Error correction", "Outer code strength", "C2 parity: a track with only a few bad blocks is fully corrected.",
            ParamKind.Float, 0, 1, "0.00", p => p.OuterEcc, (p, v) => p.OuterEcc = v),

        new("Concealment", "Concealment", "What the deck does with an unrecoverable macroblock.",
            ParamKind.Enum, 0, 5, "", p => p.Mode, (p, v) => p.Mode = (int)v, ConcealModes),
        new("Concealment", "Seed", "",
            ParamKind.Int, 1, 999, "0", p => p.Seed, (p, v) => p.Seed = (int)v),
        new("Concealment", "Show error map", "Tint concealed macroblocks red (preview only).",
            ParamKind.Bool, 0, 1, "", p => B(p.ShowMap), (p, v) => p.ShowMap = v > 0.5),
    };

    // ---- presets ----
    public static readonly (string Name, Action<GlitchParams> Apply)[] Presets =
    {
        ("Clean (bypass)", p => { }),
        ("Clogged head A", p => { p.HeadAClog = 1; p.ClogFlicker = 0.15; }),
        ("Clogged head B (partial)", p => { p.HeadBClog = 0.55; p.ClogFlicker = 0.6; p.Fluctuation = 0.12; }),
        ("Dirty heads, intermittent", p => { p.HeadAClog = 0.55; p.HeadBClog = 0.42; p.ClogFlicker = 0.85; p.DebrisRate = 10; p.DebrisDuration = 6; p.Fluctuation = 0.12; }),
        ("Debris strikes", p => { p.DebrisRate = 30; p.DebrisDuration = 4; p.Fluctuation = 0.12; }),
        ("TG3 entrance misaligned", p => { p.Entrance = 0.95; p.Wobble = 0.3; p.WobbleSpeed = 0.35; }),
        ("TG5 exit misaligned", p => { p.Exit = 0.95; p.Wobble = 0.3; p.WobbleSpeed = 0.35; }),
        ("Guide sweep (diagonal)", p => { p.Mapping = 1; p.Entrance = 1.0; p.Wobble = 0.55; p.WobbleSpeed = 0.25; p.Fluctuation = 0.12; }),
        ("Tracking drift", p => { p.TrackingOffset = 0.38; p.Wobble = 0.35; p.WobbleSpeed = 0.3; p.Jitter = 0.03; p.Fluctuation = 0.1; }),
        ("Scratched tape", p => { p.ScratchDepth = 0.9; p.ScratchWidth = 0.03; p.DropoutRate = 3; p.DropoutLength = 0.02; }),
        ("Worn tape, dropouts", p => { p.DropoutRate = 8; p.DropoutLength = 0.06; p.Fluctuation = 0.16; }),
        ("Dying deck", p => { p.HeadAClog = 0.35; p.HeadBClog = 0.22; p.ClogFlicker = 0.7; p.Entrance = 0.6; p.Wobble = 0.4;
                              p.DebrisRate = 8; p.DropoutRate = 4; p.TrackLoss = 0.02; p.Mode = 5; p.Fluctuation = 0.15; }),
    };

    static readonly JsonSerializerOptions Json = new() { IncludeFields = true, WriteIndented = true };
    public string ToJson() => JsonSerializer.Serialize(this, Json);
    public static GlitchParams FromJson(string s) => JsonSerializer.Deserialize<GlitchParams>(s, Json) ?? new GlitchParams();
}
