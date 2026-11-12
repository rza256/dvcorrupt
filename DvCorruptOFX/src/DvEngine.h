// DV Corrupt engine: DV tape / head / RF-envelope model + deck error concealment.
// Port of the DvCorrupt .NET app core (Core/*.cs). Pixel buffers are top-down, tightly
// packed RGBA with component type uint8_t, uint16_t or float.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dvc {

// ------------------------------------------------------------------ hashing / noise
inline uint32_t mix(uint32_t a)
{
    a ^= a >> 16; a *= 0x7feb352dU;
    a ^= a >> 15; a *= 0x846ca68bU;
    a ^= a >> 16;
    return a;
}

inline uint32_t hash(int seed, int64_t a, int b = 0, int c = 0)
{
    uint32_t h = mix((uint32_t)seed ^ 0x9E3779B9U);
    h = mix(h ^ (uint32_t)a);
    h = mix(h ^ (uint32_t)(a >> 32) ^ 0x85EBCA6BU);
    h = mix(h ^ (uint32_t)b);
    h = mix(h ^ ((uint32_t)c * 0xC2B2AE35U));
    return h;
}

inline float frand(int seed, int64_t a, int b = 0, int c = 0)
{
    return (hash(seed, a, b, c) >> 8) * (1.0f / 16777216.0f);
}

inline float valueNoise(int seed, double x, int salt)
{
    double fl = std::floor(x);
    int64_t i = (int64_t)fl;
    float t = (float)(x - fl);
    t = t * t * (3 - 2 * t);
    float a = frand(seed, i, salt, 77), b = frand(seed, i + 1, salt, 77);
    return a + (b - a) * t;
}

template <class T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ------------------------------------------------------------------ parameters
struct Params
{
    // video heads
    double HeadAClog = 0, HeadBClog = 0, ClogFlicker = 0.5, DebrisRate = 0, DebrisDuration = 8;
    // tape path / tracking
    double Entrance = 0, Exit = 0, TrackingOffset = 0, Wobble = 0, WobbleSpeed = 0.5, Jitter = 0;
    int Mapping = 0;
    // tape condition
    double Fluctuation = 0.08, DropoutRate = 0, DropoutLength = 0.03, DropoutDepth = 0.9;
    double ScratchDepth = 0, ScratchPos = 0.55, ScratchWidth = 0.02, TrackLoss = 0;
    // error correction
    double Threshold = 0.45, Softness = 0.04, OuterEcc = 0.35;
    // concealment
    int Mode = 0, Seed = 1;
    bool ShowMap = false;
};

enum class PKind { Double, Int, Choice, Bool };

struct ParamDef
{
    const char* group;      // group param name
    const char* name;       // OFX param name (also used by presets)
    const char* label;
    const char* hint;
    PKind kind;
    double min, max, def;
    std::vector<const char*> options;
    double Params::*d = nullptr;
    int Params::*i = nullptr;
    bool Params::*b = nullptr;

    double get(const Params& p) const { return d ? p.*d : i ? (double)(p.*i) : (p.*b ? 1.0 : 0.0); }
    void set(Params& p, double v) const
    {
        if (d) p.*d = v;
        else if (i) p.*i = (int)std::lround(v);
        else p.*b = v > 0.5;
    }
};

inline const std::vector<ParamDef>& paramDefs()
{
    static const std::vector<ParamDef> defs = [] {
        std::vector<ParamDef> v;
        auto D = [&](const char* g, const char* n, const char* l, const char* h, double mn, double mx, double Params::*m) {
            ParamDef d{g, n, l, h, PKind::Double, mn, mx, Params{}.*m, {}};
            d.d = m; v.push_back(d);
        };
        auto C = [&](const char* g, const char* n, const char* l, const char* h, std::vector<const char*> o, int Params::*m) {
            ParamDef d{g, n, l, h, PKind::Choice, 0, (double)o.size() - 1, (double)(Params{}.*m), o};
            d.i = m; v.push_back(d);
        };
        auto I = [&](const char* g, const char* n, const char* l, const char* h, double mn, double mx, int Params::*m) {
            ParamDef d{g, n, l, h, PKind::Int, mn, mx, (double)(Params{}.*m), {}};
            d.i = m; v.push_back(d);
        };
        auto B = [&](const char* g, const char* n, const char* l, const char* h, bool Params::*m) {
            ParamDef d{g, n, l, h, PKind::Bool, 0, 1, Params{}.*m ? 1.0 : 0.0, {}};
            d.b = m; v.push_back(d);
        };
        D("grpHeads", "HeadAClog", "Head A clog", "Signal loss on head A (even tracks). Full clog = every other band of blocks frozen.", 0, 1, &Params::HeadAClog);
        D("grpHeads", "HeadBClog", "Head B clog", "Signal loss on head B (odd tracks).", 0, 1, &Params::HeadBClog);
        D("grpHeads", "ClogFlicker", "Clog flicker", "How much the clog swings frame to frame around its set level.", 0, 1, &Params::ClogFlicker);
        D("grpHeads", "DebrisRate", "Debris hits / min", "Sudden shed-particle clogs that blank a head for a moment.", 0, 120, &Params::DebrisRate);
        D("grpHeads", "DebrisDuration", "Debris duration (frames)", "How long each debris clog lasts.", 1, 90, &Params::DebrisDuration);

        D("grpPath", "Entrance", "Entrance side (TG3)", "Guide misalignment where the head enters the track: start of every track is lost (audio first, then video from the top-left of each super block).", 0, 2, &Params::Entrance);
        D("grpPath", "Exit", "Exit side (TG5)", "Guide misalignment where the head leaves the track: end of every track is lost.", 0, 2, &Params::Exit);
        D("grpPath", "TrackingOffset", "Tracking offset", "Head rides off the centre of the track along its whole length.", 0, 1, &Params::TrackingOffset);
        D("grpPath", "Wobble", "Tension wobble", "Slow drift of tracking / guide error (tape stretch, back-tension).", 0, 1, &Params::Wobble);
        D("grpPath", "WobbleSpeed", "Wobble speed (Hz)", "", 0.05, 6, &Params::WobbleSpeed);
        D("grpPath", "Jitter", "Frame jitter", "Random per-frame tracking error.", 0, 0.5, &Params::Jitter);
        C("grpPath", "Mapping", "Error to screen mapping", "Authentic = real DV macroblock shuffle. Sweeps map track position straight onto the frame so guide errors grow from top-left to bottom-right.",
          {"Authentic DV shuffle", "Diagonal sweep (TL to BR)", "Raster sweep (scanline)"}, &Params::Mapping);

        D("grpTape", "Fluctuation", "RF fluctuation", "Envelope amplitude noise (spec allows ~10%).", 0, 0.6, &Params::Fluctuation);
        D("grpTape", "DropoutRate", "Dropouts / frame", "Oxide defects; each can span adjacent tracks.", 0, 30, &Params::DropoutRate);
        D("grpTape", "DropoutLength", "Dropout length", "Fraction of a track.", 0.005, 0.4, &Params::DropoutLength);
        D("grpTape", "DropoutDepth", "Dropout depth", "", 0, 1, &Params::DropoutDepth);
        D("grpTape", "ScratchDepth", "Scratch depth", "Lengthwise scratch: hits the same spot on every track, giving a regular grid of errors.", 0, 1, &Params::ScratchDepth);
        D("grpTape", "ScratchPos", "Scratch position", "Position across the tape width (= along each track).", 0, 1, &Params::ScratchPos);
        D("grpTape", "ScratchWidth", "Scratch width", "", 0.002, 0.25, &Params::ScratchWidth);
        D("grpTape", "TrackLoss", "Track sync loss", "Chance a whole track is unreadable.", 0, 0.5, &Params::TrackLoss);

        D("grpEcc", "Threshold", "Read threshold", "RF level below which Reed-Solomon can't recover a block (the digital cliff).", 0.05, 0.95, &Params::Threshold);
        D("grpEcc", "Softness", "Cliff softness", "Low = hard digital cliff; high = errors fade in gradually.", 0.005, 0.25, &Params::Softness);
        D("grpEcc", "OuterEcc", "Outer code strength", "C2 parity: a track with only a few bad blocks is fully corrected.", 0, 1, &Params::OuterEcc);

        C("grpConceal", "Mode", "Concealment", "What the deck does with an unrecoverable macroblock.",
          {"Previous frame (type 2)", "Next frame (type 4)", "Grey blocks (type F)", "Blocky / DC only", "Pink & green (no concealment)", "Mixed"}, &Params::Mode);
        I("grpConceal", "Seed", "Seed", "", 1, 999, &Params::Seed);
        B("grpConceal", "ShowMap", "Show error map", "Tint concealed macroblocks red.", &Params::ShowMap);
        return v;
    }();
    return defs;
}

struct Preset { const char* name; void (*apply)(Params&); };

inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> p = {
        {"Clean (bypass)", [](Params&) {}},
        {"Clogged head A", [](Params& p) { p.HeadAClog = 1; p.ClogFlicker = 0.15; }},
        {"Clogged head B (partial)", [](Params& p) { p.HeadBClog = 0.55; p.ClogFlicker = 0.6; p.Fluctuation = 0.12; }},
        {"Dirty heads, intermittent", [](Params& p) { p.HeadAClog = 0.55; p.HeadBClog = 0.42; p.ClogFlicker = 0.85; p.DebrisRate = 10; p.DebrisDuration = 6; p.Fluctuation = 0.12; }},
        {"Debris strikes", [](Params& p) { p.DebrisRate = 30; p.DebrisDuration = 4; p.Fluctuation = 0.12; }},
        {"TG3 entrance misaligned", [](Params& p) { p.Entrance = 0.95; p.Wobble = 0.3; p.WobbleSpeed = 0.35; }},
        {"TG5 exit misaligned", [](Params& p) { p.Exit = 0.95; p.Wobble = 0.3; p.WobbleSpeed = 0.35; }},
        {"Guide sweep (diagonal)", [](Params& p) { p.Mapping = 1; p.Entrance = 1.0; p.Wobble = 0.55; p.WobbleSpeed = 0.25; p.Fluctuation = 0.12; }},
        {"Tracking drift", [](Params& p) { p.TrackingOffset = 0.38; p.Wobble = 0.35; p.WobbleSpeed = 0.3; p.Jitter = 0.03; p.Fluctuation = 0.1; }},
        {"Scratched tape", [](Params& p) { p.ScratchDepth = 0.9; p.ScratchWidth = 0.03; p.DropoutRate = 3; p.DropoutLength = 0.02; }},
        {"Worn tape, dropouts", [](Params& p) { p.DropoutRate = 8; p.DropoutLength = 0.06; p.Fluctuation = 0.16; }},
        {"Dying deck", [](Params& p) { p.HeadAClog = 0.35; p.HeadBClog = 0.22; p.ClogFlicker = 0.7; p.Entrance = 0.6; p.Wobble = 0.4;
                                        p.DebrisRate = 8; p.DropoutRate = 4; p.TrackLoss = 0.02; p.Mode = 5; p.Fluctuation = 0.15; }},
    };
    return p;
}

// ------------------------------------------------------------------ DV macroblock layout
struct RectI { int x = 0, y = 0, w = 0, h = 0; };

constexpr int kSegments = 27;
constexpr int kVideoBlocks = 135;
constexpr int kAudioBlocks = 9;

// Same tables as IEC 61834 / ffmpeg dv_calc_mb_coordinates (25 Mbit/s, 4:1:1 NTSC and 4:2:0 PAL).
struct Layout
{
    bool pal = false;
    int tracks = 10, width = 0, height = 0;
    std::vector<RectI> rects;       // [t*135 + v]
    std::vector<float> udiag, uraster;

    void build(bool isPal, int w, int h)
    {
        static const int off[] = {2, 6, 8, 0, 4};
        static const int lstart[] = {9, 4, 13, 0, 18};
        static const int shuf3[] = {18, 9, 27, 0, 36};
        static const int serpent1[] = {0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2};
        static const int serpent2[] = {0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5};
        pal = isPal; tracks = pal ? 12 : 10; width = w; height = h;
        int rw = 720, rh = pal ? 576 : 480, mbRows = pal ? 36 : 60;
        double sx = w / (double)rw, sy = h / (double)rh;
        rects.assign(tracks * kVideoBlocks, RectI{});
        udiag.assign(tracks * kVideoBlocks, 0);
        uraster.assign(tracks * kVideoBlocks, 0);
        for (int s = 0; s < tracks; s++)
            for (int j = 0; j < kSegments; j++)
                for (int m = 0; m < 5; m++)
                {
                    int x, y, bw, bh;
                    if (pal)
                    {
                        x = (shuf3[m] + j / 3) * 16;
                        y = (serpent1[j] + ((s + off[m]) % 12) * 3) * 16;
                        bw = bh = 16;
                    }
                    else
                    {
                        int i = (s + off[m]) % 10;
                        int k = j + ((m == 1 || m == 2) ? 3 : 0);
                        int cx = lstart[m] + k / 6;
                        int cy = serpent2[k] + i * 6;
                        if (cx > 21) { cy = cy * 2 - i * 6; x = cx * 32; y = cy * 8; bw = 16; bh = 16; }
                        else { x = cx * 32; y = cy * 8; bw = 32; bh = 8; }
                    }
                    int idx = s * kVideoBlocks + j * 5 + m;
                    int x0 = clampv((int)std::lround(x * sx), 0, w), x1 = clampv((int)std::lround((x + bw) * sx), 0, w);
                    int y0 = clampv((int)std::lround(y * sy), 0, h), y1 = clampv((int)std::lround((y + bh) * sy), 0, h);
                    rects[idx] = RectI{x0, y0, x1 - x0, y1 - y0};
                    double cxn = (x + bw / 2.0) / rw, cyn = (y + bh / 2.0) / rh;
                    udiag[idx] = (float)((cxn + cyn) / 2);
                    uraster[idx] = (float)((std::floor(cyn * mbRows) + cxn) / mbRows);
                }
    }

    int coverageErrors() const
    {
        std::vector<uint8_t> cov((size_t)width * height, 0);
        for (auto& r : rects)
            for (int y = r.y; y < r.y + r.h; y++)
                for (int x = r.x; x < r.x + r.w; x++) cov[(size_t)y * width + x]++;
        int bad = 0;
        for (auto c : cov) if (c != 1) bad++;
        return bad;
    }
};

// ------------------------------------------------------------------ tape / RF model
constexpr float kAudioStart = 0.04f, kAudioEnd = 0.14f, kVideoStart = 0.16f, kVideoEnd = 0.94f;
constexpr int kEnvRes = 96;
inline float videoBlockPos(int v) { return kVideoStart + (kVideoEnd - kVideoStart) * (2 + v + 0.5f) / 149.f; }
inline float audioBlockPos(int b) { return kAudioStart + (kAudioEnd - kAudioStart) * (b + 0.5f) / kAudioBlocks; }

struct Dropout { int t0 = 0, span = 1; float pos = 0, len = 0, depth = 0, slant = 0; };

struct FrameErrors
{
    int tracks = 0;
    std::vector<uint8_t> video, audio;
    std::vector<float> videoPos, env;
    float headGain[2] = {1, 1};
    int videoErrors = 0, audioErrors = 0;
    float threshold = 0.45f;
    Dropout drops[64];
    int dropCount = 0;

    void init(int t)
    {
        tracks = t;
        video.assign(t * kVideoBlocks, 0);
        videoPos.assign(t * kVideoBlocks, 0);
        audio.assign(t * kAudioBlocks, 0);
        env.assign(t * kEnvRes, 0);
    }
};

// Pure function of (tape frame index, params): which DIF blocks fail error correction.
inline void computeTape(int64_t f, const Params& p, const Layout& L, double tapeFps, FrameErrors& e)
{
    const int T = L.tracks;
    if (e.tracks != T) e.init(T);
    const int seed = p.Seed;
    const double time = f / tapeFps;
    const double ws = p.WobbleSpeed;
    double wn1 = valueNoise(seed, time * ws, 11) * 2 - 1;
    double wn2 = valueNoise(seed, time * ws * 1.37 + 5.1, 12) * 2 - 1;
    double wn3 = valueNoise(seed, time * ws * 0.83 + 9.7, 13) * 2 - 1;
    double jit = (frand(seed, f, 14) * 2 - 1) * p.Jitter;
    float A = (float)std::fabs(p.TrackingOffset + p.Wobble * 0.6 * wn1 + jit);
    float E = (float)std::max(0.0, p.Entrance * (1 + p.Wobble * wn2) + (p.Entrance > 0 ? jit : 0));
    float X = (float)std::max(0.0, p.Exit * (1 + p.Wobble * wn3) + (p.Exit > 0 ? jit : 0));

    for (int h = 0; h < 2; h++)
    {
        double clog = h == 0 ? p.HeadAClog : p.HeadBClog;
        double flick = 0.5 * valueNoise(seed, time * 3, 20 + h) + 0.5 * frand(seed, f, 22 + h);
        double c = clog * (1 + p.ClogFlicker * (2 * flick - 1));
        if (p.DebrisRate > 0)
        {
            int dur = std::max(1, (int)p.DebrisDuration);
            int64_t w = (f + h * 7919LL) / dur;
            double prob = p.DebrisRate / 60.0 / tapeFps * dur;
            if (frand(seed, w, 30 + h) < prob) c += 0.6 + 0.45 * frand(seed, w, 32 + h);
        }
        e.headGain[h] = (float)clampv(1 - c, 0.0, 1.0);
    }

    int n = (int)p.DropoutRate;
    if (frand(seed, f, 40) < p.DropoutRate - n) n++;
    n = std::min(n, 64);
    e.dropCount = n;
    for (int i = 0; i < n; i++)
    {
        Dropout& d = e.drops[i];
        d.t0 = (int)(frand(seed, f, 41, i) * T);
        d.pos = frand(seed, f, 42, i);
        d.len = (float)(p.DropoutLength * (0.4 + 1.2 * frand(seed, f, 43, i)));
        d.span = 1 + (int)(frand(seed, f, 44, i) * 2.6f);
        d.depth = (float)(p.DropoutDepth * (0.6 + 0.4 * frand(seed, f, 45, i)));
        d.slant = (frand(seed, f, 46, i) - 0.5f) * 0.04f;
    }

    const float sPos = (float)(p.ScratchPos + 0.008 * (valueNoise(seed, time * 2, 50) * 2 - 1));
    const float sHalf = (float)p.ScratchWidth * 0.5f, sDepth = (float)p.ScratchDepth;
    const float fluct = (float)p.Fluctuation;
    const float thr = (float)p.Threshold, soft = (float)std::max(1e-4, p.Softness);
    const int capV = (int)std::lround(p.OuterEcc * 11);
    const int capA = (int)std::lround(p.OuterEcc * 5);
    e.threshold = thr;
    e.videoErrors = e.audioErrors = 0;
    const std::vector<float>* posTab = p.Mapping == 1 ? &L.udiag : p.Mapping == 2 ? &L.uraster : nullptr;

    for (int t = 0; t < T; t++)
    {
        const float gain = e.headGain[t & 1];
        const bool lost = frand(seed, f, 60, t) < p.TrackLoss;
        const float waveOff = t * 17.3f + (float)(f % 100000) * 3.11f;

        auto envAt = [&](float pos, bool blockNoise, int salt) {
            float dev = A + E * std::pow(1 - pos, 1.2f) + X * std::pow(pos, 1.2f);
            float env = gain * clampv(1 - dev, 0.0f, 1.0f);
            for (int i = 0; i < e.dropCount; i++)
            {
                const Dropout& d = e.drops[i];
                int dt = t - d.t0;
                if (dt < 0 || dt >= d.span) continue;
                float c = d.pos + d.slant * dt;
                float dist = std::fabs(pos - c);
                if (dist < d.len * 0.5f)
                {
                    float edge = 1 - dist / (d.len * 0.5f);
                    env *= 1 - d.depth * std::min(1.0f, edge * 3);
                }
            }
            if (sDepth > 0 && std::fabs(pos - sPos) < sHalf) env *= 1 - sDepth;
            float wave = valueNoise(seed, pos * 6 + waveOff, 75) * 2 - 1;
            float noise = blockNoise ? frand(seed, f, salt, 70 + t) * 2 - 1 : 0;
            env *= 1 + fluct * (0.55f * wave + 0.45f * noise);
            return std::max(0.0f, env);
        };
        auto fails = [&](float env, int salt) {
            float pr = 1.f / (1.f + std::exp(-(thr - env) / soft));
            return frand(seed, f, salt, 90 + t) < pr;
        };

        const int vBase = t * kVideoBlocks;
        int vErr = 0;
        for (int v = 0; v < kVideoBlocks; v++)
        {
            float pos = posTab ? kVideoStart + (kVideoEnd - kVideoStart) * (*posTab)[vBase + v] : videoBlockPos(v);
            e.videoPos[vBase + v] = pos;
            bool bad = lost || fails(envAt(pos, true, v), v);
            e.video[vBase + v] = bad;
            vErr += bad;
        }
        if (!lost && vErr <= capV) { std::fill(e.video.begin() + vBase, e.video.begin() + vBase + kVideoBlocks, 0); vErr = 0; }
        e.videoErrors += vErr;

        const int aBase = t * kAudioBlocks;
        int aErr = 0;
        for (int b = 0; b < kAudioBlocks; b++)
        {
            bool bad = lost || fails(envAt(audioBlockPos(b), true, 500 + b), 500 + b);
            e.audio[aBase + b] = bad;
            aErr += bad;
        }
        if (!lost && aErr <= capA) { std::fill(e.audio.begin() + aBase, e.audio.begin() + aBase + kAudioBlocks, 0); aErr = 0; }
        e.audioErrors += aErr;

        for (int i = 0; i < kEnvRes; i++)
            e.env[t * kEnvRes + i] = lost ? 0 : envAt((i + 0.5f) / kEnvRes, false, 0);
    }
}

// ------------------------------------------------------------------ pixel ops
enum class Depth { Byte = 0, Short = 1, Float = 2 };
inline int bytesPerPixel(Depth d) { return d == Depth::Byte ? 4 : d == Depth::Short ? 8 : 16; }

template <class T> struct PixTraits;
template <> struct PixTraits<uint8_t> { static constexpr float maxv = 255.f; static uint8_t from(float v) { return (uint8_t)clampv(v * 255.f + 0.5f, 0.f, 255.f); } };
template <> struct PixTraits<uint16_t> { static constexpr float maxv = 65535.f; static uint16_t from(float v) { return (uint16_t)clampv(v * 65535.f + 0.5f, 0.f, 65535.f); } };
template <> struct PixTraits<float> { static constexpr float maxv = 1.f; static float from(float v) { return v; } };

struct FrameView
{
    uint8_t* data = nullptr;
    int w = 0, h = 0;
    Depth depth = Depth::Byte;
    size_t rowBytes() const { return (size_t)w * bytesPerPixel(depth); }
    uint8_t* row(int y) const { return data + rowBytes() * y; }
};

namespace detail {

template <class T>
void fillRect(const FrameView& d, RectI r, float rr, float gg, float bb)
{
    for (int y = r.y; y < r.y + r.h; y++)
    {
        T* p = (T*)d.row(y) + r.x * 4;
        for (int x = 0; x < r.w; x++, p += 4)
        {
            p[0] = PixTraits<T>::from(rr); p[1] = PixTraits<T>::from(gg); p[2] = PixTraits<T>::from(bb);
        }
    }
}

// DC-only reconstruction: AC coefficients lost, each 8x8 DCT block becomes flat.
template <class T>
void dcOnly(const FrameView& s, const FrameView& d, RectI r, int bw, int bh)
{
    for (int by = r.y; by < r.y + r.h; by += bh)
        for (int bx = r.x; bx < r.x + r.w; bx += bw)
        {
            int ey = std::min(by + bh, r.y + r.h), ex = std::min(bx + bw, r.x + r.w);
            double acc[3] = {0, 0, 0};
            int n = 0;
            for (int y = by; y < ey; y++)
            {
                const T* p = (const T*)s.row(y) + bx * 4;
                for (int x = bx; x < ex; x++, p += 4) { acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; n++; }
            }
            if (!n) continue;
            T c0 = (T)(acc[0] / n), c1 = (T)(acc[1] / n), c2 = (T)(acc[2] / n);
            for (int y = by; y < ey; y++)
            {
                T* p = (T*)d.row(y) + bx * 4;
                for (int x = bx; x < ex; x++, p += 4) { p[0] = c0; p[1] = c1; p[2] = c2; }
            }
        }
}

// What a decoder shows when it decodes garbage: saturated magenta / green blocks with DCT stripes.
template <class T>
void pinkGreen(const FrameView& s, const FrameView& d, RectI r, uint32_t h, int bw, int bh)
{
    const float pi = 3.14159265f;
    int sub = 0;
    for (int by = r.y; by < r.y + r.h; by += bh)
        for (int bx = r.x; bx < r.x + r.w; bx += bw, sub++)
        {
            uint32_t k = mix(h + (uint32_t)sub * 0x9E37U);
            bool pink = (((k >> 1) & 3) != 0) ? ((h >> 7) & 1) == 0 : (k & 1) == 0;
            int fx = (int)((k >> 3) & 7), fy = (int)((k >> 6) & 7);
            float dc = 0.35f + ((k >> 9) & 255) / 255.f * 0.6f;
            int ey = std::min(by + bh, r.y + r.h), ex = std::min(bx + bw, r.x + r.w);
            for (int y = by; y < ey; y++)
            {
                float cy = std::cos(pi * fy * ((y - by) + 0.5f) / bh);
                const T* sp = (const T*)s.row(y) + bx * 4;
                T* dp = (T*)d.row(y) + bx * 4;
                for (int x = bx; x < ex; x++, sp += 4, dp += 4)
                {
                    float cx = std::cos(pi * fx * ((x - bx) + 0.5f) / bw);
                    float luma = (sp[0] * 0.299f + sp[1] * 0.587f + sp[2] * 0.114f) / PixTraits<T>::maxv;
                    float v = clampv(dc + 0.35f * cx * cy, 0.f, 1.f) * (0.55f + 0.45f * clampv(luma, 0.f, 1.f));
                    if (pink) { dp[0] = PixTraits<T>::from(0.96f * v); dp[1] = PixTraits<T>::from(0.2f * v); dp[2] = PixTraits<T>::from(0.9f * v); }
                    else { dp[0] = PixTraits<T>::from(0.16f * v); dp[1] = PixTraits<T>::from(0.92f * v); dp[2] = PixTraits<T>::from(0.2f * v); }
                }
            }
        }
}

template <class T>
void tint(const FrameView& d, RectI r)
{
    for (int y = r.y; y < r.y + r.h; y++)
    {
        T* p = (T*)d.row(y) + r.x * 4;
        for (int x = 0; x < r.w; x++, p += 4)
        {
            float rr = p[0] / PixTraits<T>::maxv, gg = p[1] / PixTraits<T>::maxv, bb = p[2] / PixTraits<T>::maxv;
            p[0] = PixTraits<T>::from(rr * 0.45f + 0.55f);
            p[1] = PixTraits<T>::from(gg * 0.45f);
            p[2] = PixTraits<T>::from(bb * 0.45f);
        }
    }
}

template <class T>
void conceal(const FrameView& src, const FrameView* next, const FrameView& memory, const FrameView& out,
             const Layout& L, const FrameErrors& e, const Params& p, int64_t tf)
{
    const int bw = std::max(1, (int)std::lround(8.0 * out.w / 720));
    const int bh = std::max(1, (int)std::lround(8.0 * out.h / (L.pal ? 576 : 480)));
    const size_t bpp = sizeof(T) * 4;
    for (size_t i = 0; i < e.video.size(); i++)
    {
        if (!e.video[i]) continue;
        RectI r = L.rects[i];
        if (r.w <= 0 || r.h <= 0) continue;
        int mode = p.Mode;
        if (mode == 5)
        {
            uint32_t hsh = hash(p.Seed, tf, (int)i, 900) % 100;
            mode = hsh < 86 ? 0 : hsh < 95 ? 3 : hsh < 98 ? 4 : 2;
        }
        switch (mode)
        {
        case 0:
        case 1:
        {
            const FrameView& from = (mode == 1 && next) ? *next : memory;
            for (int y = r.y; y < r.y + r.h; y++)
                std::memcpy(out.row(y) + r.x * bpp, from.row(y) + r.x * bpp, r.w * bpp);
            break;
        }
        case 2: fillRect<T>(out, r, 0.5f, 0.5f, 0.5f); break;
        case 3: dcOnly<T>(src, out, r, bw, bh); break;
        case 4: pinkGreen<T>(src, out, r, hash(p.Seed, tf, (int)i, 901), bw, bh); break;
        }
    }
}

// RF envelope scope (like the service-manual oscilloscope check), drawn into the bottom of the frame.
template <class T>
void drawScope(const FrameView& d, const FrameErrors& e)
{
    const int H = std::max(24, d.h / 6), W = d.w;
    const int y0 = d.h - H;
    const int NT = e.tracks;
    const int mid = (H - 6) / 2, amp = std::max(1, mid - 2);
    const int thrPx = (int)(e.threshold * amp);
    auto px = [&](int x, int y, float r, float g, float b) {
        if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
        T* p = (T*)d.row(y0 + y) + x * 4;
        p[0] = PixTraits<T>::from(r); p[1] = PixTraits<T>::from(g); p[2] = PixTraits<T>::from(b); p[3] = PixTraits<T>::from(1.f);
    };
    for (int t = 0; t < NT; t++)
    {
        int x0 = t * W / NT, x1 = (t + 1) * W / NT, lw = std::max(1, x1 - x0);
        bool headA = (t & 1) == 0;
        for (int x = x0; x < x1; x++)
        {
            float pp = (x - x0 + 0.5f) / lw;
            bool aud = pp >= kAudioStart && pp < kAudioEnd, vid = pp >= kVideoStart && pp < kVideoEnd;
            float br = aud ? 0.13f : vid ? 0.09f : 0.07f, bg = aud ? 0.10f : vid ? 0.11f : 0.07f, bb = aud ? 0.16f : vid ? 0.11f : 0.08f;
            for (int y = 0; y < H; y++) px(x, y, br, bg, bb);
            float fi = pp * kEnvRes - 0.5f;
            int i0 = clampv((int)std::floor(fi), 0, kEnvRes - 1), i1 = std::min(i0 + 1, kEnvRes - 1);
            float fr = clampv(fi - i0, 0.f, 1.f);
            float env = e.env[t * kEnvRes + i0] * (1 - fr) + e.env[t * kEnvRes + i1] * fr;
            int half = (int)(std::min(env, 1.2f) * amp);
            bool below = env < e.threshold;
            for (int y = mid - half; y <= mid + half; y++)
            {
                float k = 1 - std::abs(y - mid) / (float)std::max(1, half) * 0.5f;
                if (below) px(x, y, 0.92f * k, 0.27f * k, 0.24f * k);
                else if (headA) px(x, y, 0.27f * k, 0.78f * k, 0.92f * k);
                else px(x, y, 0.96f * k, 0.63f * k, 0.27f * k);
            }
            if ((x & 3) < 2) { px(x, mid - thrPx, 0.94f, 0.86f, 0.24f); px(x, mid + thrPx, 0.94f, 0.86f, 0.24f); }
        }
        for (int y = 0; y < H; y++) px(x0, y, 0.27f, 0.27f, 0.27f);
        for (int v = 0; v < kVideoBlocks; v++)
        {
            if (!e.video[t * kVideoBlocks + v]) continue;
            int x = x0 + (int)(e.videoPos[t * kVideoBlocks + v] * lw);
            for (int y = H - 5; y < H; y++) px(x, y, 1, 0.2f, 0.2f);
        }
        for (int b = 0; b < kAudioBlocks; b++)
        {
            if (!e.audio[t * kAudioBlocks + b]) continue;
            int x = x0 + (int)(audioBlockPos(b) * lw);
            for (int y = H - 5; y < H; y++) { px(x, y, 0.9f, 0.24f, 1); px(x + 1, y, 0.9f, 0.24f, 1); }
        }
    }
}

} // namespace detail

// Concealment kinds for one macroblock, as resolved by walking back through the deck history.
enum class Fill : uint8_t { Source = 0, Next = 1, Grey = 2, Dc = 3, Pink = 4 };

// Mixed mode picks a strategy per (tape frame, block); other modes are fixed.
inline int blockMode(const Params& p, int64_t tf, int block)
{
    if (p.Mode != 5) return p.Mode;
    uint32_t h = hash(p.Seed, tf, block, 900) % 100;
    return h < 86 ? 0 : h < 95 ? 3 : h < 98 ? 4 : 2;
}

// Fill one block rect of `out` from `from` using the given kind (Source/Next copy, Grey, DC-only, Pink garbage).
inline void fillBlock(Fill kind, const FrameView& from, const FrameView& out, RectI r, const Layout& L, uint32_t h)
{
    if (r.w <= 0 || r.h <= 0) return;
    const int bw = std::max(1, (int)std::lround(8.0 * out.w / 720));
    const int bh = std::max(1, (int)std::lround(8.0 * out.h / (L.pal ? 576 : 480)));
    const size_t bpp = bytesPerPixel(out.depth);
    switch (kind)
    {
    case Fill::Source:
    case Fill::Next:
        for (int y = r.y; y < r.y + r.h; y++) std::memcpy(out.row(y) + r.x * bpp, from.row(y) + r.x * bpp, r.w * bpp);
        break;
    case Fill::Grey:
        if (out.depth == Depth::Byte) detail::fillRect<uint8_t>(out, r, .5f, .5f, .5f);
        else if (out.depth == Depth::Short) detail::fillRect<uint16_t>(out, r, .5f, .5f, .5f);
        else detail::fillRect<float>(out, r, .5f, .5f, .5f);
        break;
    case Fill::Dc:
        if (out.depth == Depth::Byte) detail::dcOnly<uint8_t>(from, out, r, bw, bh);
        else if (out.depth == Depth::Short) detail::dcOnly<uint16_t>(from, out, r, bw, bh);
        else detail::dcOnly<float>(from, out, r, bw, bh);
        break;
    case Fill::Pink:
        if (out.depth == Depth::Byte) detail::pinkGreen<uint8_t>(from, out, r, h, bw, bh);
        else if (out.depth == Depth::Short) detail::pinkGreen<uint16_t>(from, out, r, h, bw, bh);
        else detail::pinkGreen<float>(from, out, r, h, bw, bh);
        break;
    }
}

// out must already contain a copy of src.
inline void conceal(const FrameView& src, const FrameView* next, const FrameView& memory, const FrameView& out,
                    const Layout& L, const FrameErrors& e, const Params& p, int64_t tf)
{
    switch (out.depth)
    {
    case Depth::Byte: detail::conceal<uint8_t>(src, next, memory, out, L, e, p, tf); break;
    case Depth::Short: detail::conceal<uint16_t>(src, next, memory, out, L, e, p, tf); break;
    case Depth::Float: detail::conceal<float>(src, next, memory, out, L, e, p, tf); break;
    }
}

inline void overlayMap(const FrameView& d, const Layout& L, const FrameErrors& e)
{
    for (size_t i = 0; i < e.video.size(); i++)
    {
        if (!e.video[i]) continue;
        switch (d.depth)
        {
        case Depth::Byte: detail::tint<uint8_t>(d, L.rects[i]); break;
        case Depth::Short: detail::tint<uint16_t>(d, L.rects[i]); break;
        case Depth::Float: detail::tint<float>(d, L.rects[i]); break;
        }
    }
}

inline void overlayScope(const FrameView& d, const FrameErrors& e)
{
    switch (d.depth)
    {
    case Depth::Byte: detail::drawScope<uint8_t>(d, e); break;
    case Depth::Short: detail::drawScope<uint16_t>(d, e); break;
    case Depth::Float: detail::drawScope<float>(d, e); break;
    }
}

} // namespace dvc
