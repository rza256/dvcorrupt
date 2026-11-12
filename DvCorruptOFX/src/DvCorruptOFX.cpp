// DV Corrupt – OpenFX video filter for VEGAS Pro (and other OFX hosts).
// Simulates a DV deck playing a damaged tape: clogged heads, misaligned tape guides,
// dropouts, scratches -> lost DIF blocks -> deck error concealment.
//
// Rendering is a pure function of the frame number: a concealed macroblock walks back through
// the (deterministic) error maps of earlier tape frames to the last frame where it read cleanly
// and copies that frame's pixels. So the result does not depend on render order, field passes,
// prefetch threads or how many instances the host creates.
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include "DvEngine.h"

#define PLUGIN_ID "com.dvcorrupt:DVCorrupt"
#define PLUGIN_LABEL "DV Corrupt (Tape Error Concealment)"
#define PLUGIN_GROUP "DV Corrupt"

static OfxHost* gHost = nullptr;
static const OfxPropertySuiteV1* gProp = nullptr;
static const OfxImageEffectSuiteV1* gEffect = nullptr;
static const OfxParameterSuiteV1* gParam = nullptr;

// ------------------------------------------------------------------ optional debug log
// Set the environment variable DVCORRUPT_LOG to a file path to log host calls.
static FILE* gLog = nullptr;
static std::mutex gLogMtx;
static int gLogLines = 0;
static void logf(const char* fmt, ...)
{
    if (!gLog) return;
    std::lock_guard<std::mutex> lk(gLogMtx);
    if (gLogLines++ > 20000) return;
    fprintf(gLog, "[%5lu] ", GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(gLog, fmt, ap);
    va_end(ap);
    fputc('\n', gLog);
    fflush(gLog);
}

// ------------------------------------------------------------------ extra (non-engine) params
static const char* kSystem = "System";
static const char* kMemory = "DeckMemory";
static const char* kShowScope = "ShowScope";

struct Group { const char* name; const char* label; bool open; };
static const Group kGroups[] = {
    {"grpDeck", "Deck / format", true},
    {"grpHeads", "Video heads", true},
    {"grpPath", "Tape path / tracking", true},
    {"grpTape", "Tape condition", false},
    {"grpEcc", "Error correction", false},
    {"grpConceal", "Concealment", true},
};

// ------------------------------------------------------------------ instance
using Buf = std::shared_ptr<std::vector<uint8_t>>;

struct Instance
{
    OfxImageEffectHandle effect = nullptr;
    OfxImageClipHandle srcClip = nullptr, dstClip = nullptr;
    std::vector<OfxParamHandle> params;   // parallel to dvc::paramDefs()
    OfxParamHandle pSystem = nullptr, pMemory = nullptr, pShowScope = nullptr;

    std::atomic<uint64_t> gen{1};
    std::mutex mtx;

    // caches (guarded by mtx), all invalidated when gen / format changes
    uint64_t cacheGen = 0;
    int w = 0, h = 0, step = 1;
    bool pal = false;
    double fps = 30000.0 / 1001.0;
    dvc::Depth depth = dvc::Depth::Byte;
    dvc::Layout layout;
    dvc::FrameErrors fe;
    std::unordered_map<int64_t, std::vector<uint8_t>> errMaps;   // tape frame -> lost-block flags
    std::list<std::pair<int64_t, Buf>> srcCache;                  // most recent at front
    size_t srcBytes = 0;
    std::vector<uint8_t> out, display;
};

static Instance* getInstance(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    void* p = nullptr;
    gProp->propGetPointer(props, kOfxPropInstanceData, 0, &p);
    return (Instance*)p;
}

static int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// ------------------------------------------------------------------ describe
static OfxStatus describe(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetString(props, kOfxPropLabel, 0, PLUGIN_LABEL);
    gProp->propSetString(props, kOfxPropShortLabel, 0, "DV Corrupt");
    gProp->propSetString(props, kOfxPropLongLabel, 0, PLUGIN_LABEL);
    gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, PLUGIN_GROUP);
    gProp->propSetString(props, kOfxPropPluginDescription, 0,
                         "Simulates a DV deck playing a damaged MiniDV tape: clogged heads, misaligned tape guides (TG3/TG5), "
                         "tracking drift, dropouts and scratches drive a per-track RF envelope; blocks that fail error correction "
                         "are concealed like a real deck using the IEC 61834 macroblock shuffle.");
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);
    gProp->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0);
    gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe);
    gProp->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0, 0);
    return kOfxStatOK;
}

static void defineDouble(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
                         double mn, double mx, double def)
{
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, kOfxParamTypeDouble, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetString(p, kOfxParamPropScriptName, 0, name);
    if (hint && *hint) gProp->propSetString(p, kOfxParamPropHint, 0, hint);
    gProp->propSetString(p, kOfxParamPropParent, 0, parent);
    gProp->propSetString(p, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, def);
    gProp->propSetDouble(p, kOfxParamPropMin, 0, mn);
    gProp->propSetDouble(p, kOfxParamPropMax, 0, mx);
    gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, mn);
    gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, mx);
    gProp->propSetDouble(p, kOfxParamPropIncrement, 0, (mx - mn) / 100.0);
    gProp->propSetInt(p, kOfxParamPropDigits, 0, 3);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 1);
}

static void defineInt(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
                      int mn, int mx, int def, bool animates)
{
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, kOfxParamTypeInteger, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetString(p, kOfxParamPropScriptName, 0, name);
    if (hint && *hint) gProp->propSetString(p, kOfxParamPropHint, 0, hint);
    gProp->propSetString(p, kOfxParamPropParent, 0, parent);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
    gProp->propSetInt(p, kOfxParamPropMin, 0, mn);
    gProp->propSetInt(p, kOfxParamPropMax, 0, mx);
    gProp->propSetInt(p, kOfxParamPropDisplayMin, 0, mn);
    gProp->propSetInt(p, kOfxParamPropDisplayMax, 0, mx);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, animates ? 1 : 0);
}

static void defineChoice(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
                         const std::vector<const char*>& opts, int def)
{
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, kOfxParamTypeChoice, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetString(p, kOfxParamPropScriptName, 0, name);
    if (hint && *hint) gProp->propSetString(p, kOfxParamPropHint, 0, hint);
    gProp->propSetString(p, kOfxParamPropParent, 0, parent);
    for (int i = 0; i < (int)opts.size(); i++) gProp->propSetString(p, kOfxParamPropChoiceOption, i, opts[i]);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}

static void defineBool(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent, bool def)
{
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, kOfxParamTypeBoolean, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetString(p, kOfxParamPropScriptName, 0, name);
    if (hint && *hint) gProp->propSetString(p, kOfxParamPropHint, 0, hint);
    gProp->propSetString(p, kOfxParamPropParent, 0, parent);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def ? 1 : 0);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}

static OfxStatus describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle /*inArgs*/)
{
    OfxPropertySetHandle cp;
    gEffect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gProp->propSetInt(cp, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    gProp->propSetInt(cp, kOfxImageClipPropIsMask, 0, 0);
    gProp->propSetString(cp, kOfxImageClipPropFieldExtraction, 0, kOfxImageFieldBoth);
    gProp->propSetInt(cp, kOfxImageEffectPropSupportsTiles, 0, 0);

    gEffect->clipDefine(effect, kOfxImageEffectOutputClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gProp->propSetInt(cp, kOfxImageEffectPropSupportsTiles, 0, 0);

    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);
    for (auto& g : kGroups)
    {
        OfxPropertySetHandle p;
        gParam->paramDefine(ps, kOfxParamTypeGroup, g.name, &p);
        gProp->propSetString(p, kOfxPropLabel, 0, g.label);
        gProp->propSetInt(p, kOfxParamPropGroupOpen, 0, g.open ? 1 : 0);
    }

    defineChoice(ps, kSystem, "DV system", "Track structure of the simulated tape. Auto picks PAL for 25/50 fps, otherwise NTSC.", "grpDeck",
                 {"Auto (from frame rate)", "NTSC 525/60 (10 tracks/frame)", "PAL 625/50 (12 tracks/frame)"}, 0);
    {
        OfxPropertySetHandle p;
        gParam->paramDefine(ps, kOfxParamTypeDouble, kMemory, &p);
        gProp->propSetString(p, kOfxPropLabel, 0, "Deck memory (seconds)");
        gProp->propSetString(p, kOfxParamPropScriptName, 0, kMemory);
        gProp->propSetString(p, kOfxParamPropHint, 0,
                             "How far back a concealed block looks for its last clean picture. A block that stays dead longer than "
                             "this refreshes to a newer frozen picture once per period.");
        gProp->propSetString(p, kOfxParamPropParent, 0, "grpDeck");
        gProp->propSetDouble(p, kOfxParamPropDefault, 0, 10.0);
        gProp->propSetDouble(p, kOfxParamPropMin, 0, 0.5);
        gProp->propSetDouble(p, kOfxParamPropMax, 0, 60.0);
        gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0.5);
        gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, 60.0);
        gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
    }
    defineBool(ps, kShowScope, "Show RF scope", "Draws the per-track RF envelope (entrance left, exit right) over the bottom of the frame.", "grpDeck", false);

    for (auto& d : dvc::paramDefs())
    {
        switch (d.kind)
        {
        case dvc::PKind::Double: defineDouble(ps, d.name, d.label, d.hint, d.group, d.min, d.max, d.def); break;
        case dvc::PKind::Int: defineInt(ps, d.name, d.label, d.hint, d.group, (int)d.min, (int)d.max, (int)d.def, false); break;
        case dvc::PKind::Choice: defineChoice(ps, d.name, d.label, d.hint, d.group, d.options, (int)d.def); break;
        case dvc::PKind::Bool: defineBool(ps, d.name, d.label, d.hint, d.group, d.def > 0.5); break;
        }
    }
    return kOfxStatOK;
}

// ------------------------------------------------------------------ instance lifetime
static OfxStatus createInstance(OfxImageEffectHandle effect)
{
    auto* I = new Instance();
    I->effect = effect;
    gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &I->srcClip, nullptr);
    gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &I->dstClip, nullptr);
    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);
    for (auto& d : dvc::paramDefs())
    {
        OfxParamHandle h = nullptr;
        gParam->paramGetHandle(ps, d.name, &h, nullptr);
        I->params.push_back(h);
    }
    gParam->paramGetHandle(ps, kSystem, &I->pSystem, nullptr);
    gParam->paramGetHandle(ps, kMemory, &I->pMemory, nullptr);
    gParam->paramGetHandle(ps, kShowScope, &I->pShowScope, nullptr);

    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetPointer(props, kOfxPropInstanceData, 0, I);
    logf("createInstance %p", (void*)I);
    return kOfxStatOK;
}

static OfxStatus destroyInstance(OfxImageEffectHandle effect)
{
    Instance* I = getInstance(effect);
    logf("destroyInstance %p", (void*)I);
    delete I;
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetPointer(props, kOfxPropInstanceData, 0, nullptr);
    return kOfxStatOK;
}

static OfxStatus instanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
    Instance* I = getInstance(effect);
    if (!I) return kOfxStatReplyDefault;
    char* reason = nullptr;
    char* name = nullptr;
    if (inArgs)
    {
        gProp->propGetString(inArgs, kOfxPropChangeReason, 0, &reason);
        gProp->propGetString(inArgs, kOfxPropName, 0, &name);
    }
    if (!reason || std::strcmp(reason, kOfxChangeTime) != 0) I->gen++;
    logf("instanceChanged %p name=%s reason=%s", (void*)I, name ? name : "?", reason ? reason : "?");
    return kOfxStatOK;
}

// ------------------------------------------------------------------ helpers
static int getInt(OfxParamHandle h, double t, int def)
{
    if (!h) return def;
    int v = def;
    if (gParam->paramGetValueAtTime(h, t, &v) != kOfxStatOK) return def;
    return v;
}

static double getDouble(OfxParamHandle h, double t, double def)
{
    if (!h) return def;
    double v = def;
    if (gParam->paramGetValueAtTime(h, t, &v) != kOfxStatOK) return def;
    return v;
}

static dvc::Params readParams(Instance* I, double t)
{
    dvc::Params p;
    const auto& defs = dvc::paramDefs();
    for (size_t i = 0; i < defs.size(); i++)
    {
        OfxParamHandle h = I->params[i];
        if (!h) continue;
        if (defs[i].kind == dvc::PKind::Double)
        {
            double v = defs[i].def;
            if (gParam->paramGetValueAtTime(h, t, &v) == kOfxStatOK) defs[i].set(p, v);
        }
        else
        {
            int v = (int)defs[i].def;
            if (gParam->paramGetValueAtTime(h, t, &v) == kOfxStatOK) defs[i].set(p, v);
        }
    }
    return p;
}

static bool depthFromString(const char* s, dvc::Depth& d)
{
    if (!s) return false;
    if (!std::strcmp(s, kOfxBitDepthByte)) { d = dvc::Depth::Byte; return true; }
    if (!std::strcmp(s, kOfxBitDepthShort)) { d = dvc::Depth::Short; return true; }
    if (!std::strcmp(s, kOfxBitDepthFloat)) { d = dvc::Depth::Float; return true; }
    return false;
}

struct ImageInfo
{
    uint8_t* data = nullptr;
    int bounds[4] = {0, 0, 0, 0};
    int rowBytes = 0;
    dvc::Depth depth = dvc::Depth::Byte;
    std::string field;
    int w() const { return bounds[2] - bounds[0]; }
    int h() const { return bounds[3] - bounds[1]; }
};

static bool imageInfo(OfxPropertySetHandle img, ImageInfo& ii)
{
    void* data = nullptr;
    char* comps = nullptr;
    char* depth = nullptr;
    char* field = nullptr;
    if (gProp->propGetPointer(img, kOfxImagePropData, 0, &data) != kOfxStatOK || !data) return false;
    gProp->propGetIntN(img, kOfxImagePropBounds, 4, ii.bounds);
    gProp->propGetInt(img, kOfxImagePropRowBytes, 0, &ii.rowBytes);
    gProp->propGetString(img, kOfxImageEffectPropComponents, 0, &comps);
    gProp->propGetString(img, kOfxImageEffectPropPixelDepth, 0, &depth);
    if (gProp->propGetString(img, kOfxImagePropField, 0, &field) == kOfxStatOK && field) ii.field = field;
    if (!comps || std::strcmp(comps, kOfxImageComponentRGBA) != 0) return false;
    if (!depthFromString(depth, ii.depth)) return false;
    ii.data = (uint8_t*)data;
    return ii.w() > 0 && ii.h() > 0;
}

// OFX images are bottom-up; the engine works top-down.
static void copyToTopDown(const ImageInfo& ii, std::vector<uint8_t>& buf)
{
    const int w = ii.w(), h = ii.h();
    const size_t rb = (size_t)w * dvc::bytesPerPixel(ii.depth);
    buf.resize(rb * h);
    for (int y = 0; y < h; y++)
    {
        const uint8_t* srow = ii.data + (ptrdiff_t)ii.rowBytes * (h - 1 - y);
        std::memcpy(buf.data() + rb * y, srow, rb);
    }
}

static double sourceFps(Instance* I)
{
    OfxPropertySetHandle cp;
    double fps = 0;
    if (gEffect->clipGetPropertySet(I->srcClip, &cp) == kOfxStatOK)
        gProp->propGetDouble(cp, kOfxImageEffectPropFrameRate, 0, &fps);
    if (fps <= 1)
    {
        OfxPropertySetHandle ep;
        gEffect->getPropertySet(I->effect, &ep);
        gProp->propGetDouble(ep, kOfxImageEffectPropFrameRate, 0, &fps);
    }
    return fps > 1 ? fps : 30000.0 / 1001.0;
}

static int64_t clipFirstFrame(Instance* I)
{
    OfxPropertySetHandle cp;
    double range[2] = {0, 0};
    if (gEffect->clipGetPropertySet(I->srcClip, &cp) == kOfxStatOK)
        gProp->propGetDoubleN(cp, kOfxImageEffectPropFrameRange, 2, range);
    return (int64_t)std::floor(range[0] + 1e-3);
}

// DV system (NTSC/PAL) and output frames per tape frame (2 for 59.94p / 50p double-rate footage).
static void tapeFormat(Instance* I, double time, double fps, bool& pal, int& step)
{
    const int sys = getInt(I->pSystem, time, 0);
    pal = sys == 2 || (sys == 0 && (std::fabs(fps - 25) < 0.1 || std::fabs(fps - 50) < 0.1));
    step = fps > (pal ? 26.0 : 31.0) ? std::max(1, (int)std::lround(fps / (pal ? 25.0 : 30000.0 / 1001.0))) : 1;
}

// Host times can be fractional (e.g. second field at t+0.5); all of a frame maps to one index.
static int64_t frameIndex(double time) { return (int64_t)std::floor(time + 1e-3); }

// How many output frames a concealed block may look back (a multiple of the tape step).
static int64_t memoryFrames(Instance* I, double time, double fps, int step)
{
    double sec = getDouble(I->pMemory, time, 10.0);
    int64_t n = (int64_t)std::llround(dvc::clampv(sec, 0.5, 60.0) * fps);
    n = std::max<int64_t>(step, n - n % step);
    return n;
}

// Lost-block flags for one tape frame (computed with the parameters at that frame, then cached).
static const std::vector<uint8_t>& errMap(Instance* I, int64_t tf)
{
    auto it = I->errMaps.find(tf);
    if (it != I->errMaps.end()) return it->second;
    if (I->errMaps.size() > 6000) I->errMaps.clear();
    const dvc::Params p = readParams(I, (double)(tf * I->step));
    dvc::computeTape(tf, p, I->layout, I->fps / I->step, I->fe);
    return I->errMaps.emplace(tf, I->fe.video).first->second;
}

static Buf getSource(Instance* I, int64_t f)
{
    for (auto it = I->srcCache.begin(); it != I->srcCache.end(); ++it)
        if (it->first == f)
        {
            I->srcCache.splice(I->srcCache.begin(), I->srcCache, it);
            return I->srcCache.front().second;
        }
    OfxPropertySetHandle img = nullptr;
    OfxStatus st = gEffect->clipGetImage(I->srcClip, (double)f, nullptr, &img);
    if (st != kOfxStatOK || !img)
    {
        logf("  fetch t=%lld FAILED (%d)", (long long)f, st);
        return nullptr;
    }
    ImageInfo ii;
    Buf b;
    if (imageInfo(img, ii) && ii.w() == I->w && ii.h() == I->h && ii.depth == I->depth)
    {
        b = std::make_shared<std::vector<uint8_t>>();
        copyToTopDown(ii, *b);
    }
    else
        logf("  fetch t=%lld wrong format %dx%d", (long long)f, ii.w(), ii.h());
    gEffect->clipReleaseImage(img);
    if (!b) return nullptr;
    I->srcCache.emplace_front(f, b);
    I->srcBytes += b->size();
    const size_t maxBytes = 768ull << 20;
    while (I->srcCache.size() > 4 && (I->srcBytes > maxBytes || I->srcCache.size() > 400))
    {
        I->srcBytes -= I->srcCache.back().second->size();
        I->srcCache.pop_back();
    }
    return b;
}

static void putSource(Instance* I, int64_t f, const std::vector<uint8_t>& data)
{
    for (auto& e : I->srcCache)
        if (e.first == f) return;
    auto b = std::make_shared<std::vector<uint8_t>>(data);
    I->srcCache.emplace_front(f, b);
    I->srcBytes += b->size();
}

struct Resolved { dvc::Fill kind; int64_t frame; };

// Walk back from frame f to find what the deck shows for block i (pure function of f).
static Resolved resolveBlock(Instance* I, int i, int64_t f, int64_t first, int64_t mem, const dvc::Params& p)
{
    const int step = I->step;
    const int64_t firstSame = first + (((f - first) % step) + step) % step;   // earliest frame of the same field phase
    const int64_t lo = std::max(firstSame, f - mem);
    for (int64_t h = f; h >= lo; h -= step)
    {
        const int64_t tf = floorDiv(h, step);
        if (!errMap(I, tf)[i]) return {dvc::Fill::Source, h};
        const int m = dvc::blockMode(p, tf, i);
        if (m != 0) return {(dvc::Fill)m, h};
    }
    // dead for the whole memory window: freeze on a grid-aligned frame so it stays still
    if (f - mem < firstSame) return {dvc::Fill::Source, firstSame};
    return {dvc::Fill::Source, firstSame + floorDiv(f - mem - firstSame, mem) * mem};
}

// ------------------------------------------------------------------ render
static OfxStatus getFramesNeeded(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
    Instance* I = getInstance(effect);
    if (!I) return kOfxStatReplyDefault;
    double t = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &t);
    const double fps = sourceFps(I);
    bool pal;
    int step;
    tapeFormat(I, t, fps, pal, step);
    const int64_t f = frameIndex(t);
    const int64_t mem = memoryFrames(I, t, fps, step);
    double range[2] = {(double)(f - 2 * mem), (double)(f + step)};
    gProp->propSetDoubleN(outArgs, "OfxImageClipPropFrameRange_" kOfxImageEffectSimpleSourceClipName, 2, range);
    logf("framesNeeded %p t=%.4f -> [%g, %g]", (void*)I, t, range[0], range[1]);
    return kOfxStatOK;
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
    Instance* I = getInstance(effect);
    if (!I) return kOfxStatFailed;
    double time = 0;
    int rw[4] = {0, 0, 0, 0};
    double rs[2] = {1, 1};
    char* fieldToRender = nullptr;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    gProp->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, rw);
    gProp->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, rs);
    gProp->propGetString(inArgs, kOfxImageEffectPropFieldToRender, 0, &fieldToRender);

    OfxPropertySetHandle dstImg = nullptr, srcImg = nullptr;
    if (gEffect->clipGetImage(I->dstClip, time, nullptr, &dstImg) != kOfxStatOK || !dstImg) return kOfxStatFailed;
    if (gEffect->clipGetImage(I->srcClip, time, nullptr, &srcImg) != kOfxStatOK || !srcImg)
    {
        gEffect->clipReleaseImage(dstImg);
        return kOfxStatFailed;
    }

    OfxStatus status = kOfxStatOK;
    ImageInfo si, di;
    if (!imageInfo(srcImg, si) || !imageInfo(dstImg, di) || si.depth != di.depth)
    {
        gEffect->clipReleaseImage(srcImg);
        gEffect->clipReleaseImage(dstImg);
        return kOfxStatErrImageFormat;
    }
    logf("render %p t=%.4f field=%s scale=%.3fx%.3f src=[%d %d %d %d](%s) dst=[%d %d %d %d] rw=[%d %d %d %d]", (void*)I, time,
         fieldToRender ? fieldToRender : "?", rs[0], rs[1], si.bounds[0], si.bounds[1], si.bounds[2], si.bounds[3], si.field.c_str(),
         di.bounds[0], di.bounds[1], di.bounds[2], di.bounds[3], rw[0], rw[1], rw[2], rw[3]);

    {
        std::lock_guard<std::mutex> lock(I->mtx);
        const int w = si.w(), h = si.h();
        const double fps = sourceFps(I);
        bool pal;
        int step;
        tapeFormat(I, time, fps, pal, step);
        const uint64_t gen = I->gen.load();

        if (I->cacheGen != gen || I->w != w || I->h != h || I->depth != si.depth || I->pal != pal || I->step != step ||
            std::fabs(I->fps - fps) > 1e-6)
        {
            I->errMaps.clear();
            I->srcCache.clear();
            I->srcBytes = 0;
            I->cacheGen = gen;
            I->w = w; I->h = h; I->depth = si.depth; I->pal = pal; I->step = step; I->fps = fps;
            I->layout.build(pal, w, h);
        }

        const int64_t f = frameIndex(time);
        const int64_t tf = floorDiv(f, step);
        const int64_t first = clipFirstFrame(I);
        const int64_t mem = memoryFrames(I, time, fps, step);
        const dvc::Params pNow = readParams(I, (double)(tf * step));

        copyToTopDown(si, I->out);
        putSource(I, f, I->out);
        const std::vector<uint8_t> lost = errMap(I, tf);   // copy: errMap may rehash while resolving

        // resolve every lost block, then fetch each distinct source frame once
        std::vector<std::pair<int, Resolved>> todo;
        std::map<int64_t, Buf> frames;
        for (int i = 0; i < (int)lost.size(); i++)
        {
            if (!lost[i]) continue;
            Resolved r = resolveBlock(I, i, f, first, mem, pNow);
            todo.push_back({i, r});
            int64_t need = r.kind == dvc::Fill::Next ? r.frame + step : r.frame;
            if (r.kind != dvc::Fill::Grey && !frames.count(need)) frames[need] = nullptr;
        }
        for (auto& kv : frames)
            if (kv.first != f) kv.second = getSource(I, kv.first);

        dvc::FrameView ov{I->out.data(), w, h, si.depth};
        std::vector<uint8_t> curCopy;   // pristine current frame for blocks resolved to "now" (DC / pink of f)
        bool needCur = false;
        for (auto& t : todo)
            if (t.second.kind != dvc::Fill::Grey && (t.second.kind == dvc::Fill::Next ? t.second.frame + step : t.second.frame) == f) needCur = true;
        if (needCur) copyToTopDown(si, curCopy);
        dvc::FrameView cv{needCur ? curCopy.data() : nullptr, w, h, si.depth};

        for (auto& t : todo)
        {
            const Resolved& r = t.second;
            int64_t need = r.kind == dvc::Fill::Next ? r.frame + step : r.frame;
            dvc::FrameView from = cv;
            if (r.kind != dvc::Fill::Grey && need != f)
            {
                Buf b = frames[need];
                if (!b && r.kind == dvc::Fill::Next) b = getSource(I, r.frame);   // no next frame: fall back
                if (!b) continue;   // source unavailable: leave the live picture
                from = dvc::FrameView{b->data(), w, h, si.depth};
            }
            dvc::fillBlock(r.kind, from, ov, I->layout.rects[t.first], I->layout, dvc::hash(pNow.Seed, floorDiv(r.frame, step), t.first, 901));
        }

        const std::vector<uint8_t>* outBuf = &I->out;
        const bool showScope = getInt(I->pShowScope, time, 0) != 0;
        if (pNow.ShowMap || showScope)
        {
            dvc::computeTape(tf, pNow, I->layout, fps / step, I->fe);
            I->display = I->out;
            dvc::FrameView dv{I->display.data(), w, h, si.depth};
            if (pNow.ShowMap) dvc::overlayMap(dv, I->layout, I->fe);
            if (showScope) dvc::overlayScope(dv, I->fe);
            outBuf = &I->display;
        }

        // write the render window (dst coords) from our top-down buffer (src coords)
        const size_t bpp = dvc::bytesPerPixel(si.depth);
        const size_t rb = (size_t)w * bpp;
        const int x1 = std::max(rw[0], std::max(di.bounds[0], si.bounds[0]));
        const int x2 = std::min(rw[2], std::min(di.bounds[2], si.bounds[2]));
        const int y1 = std::max(rw[1], std::max(di.bounds[1], si.bounds[1]));
        const int y2 = std::min(rw[3], std::min(di.bounds[3], si.bounds[3]));
        for (int y = y1; y < y2; y++)
        {
            const int by = si.bounds[3] - 1 - y;
            const uint8_t* s = outBuf->data() + rb * by + (size_t)(x1 - si.bounds[0]) * bpp;
            uint8_t* d = di.data + (ptrdiff_t)di.rowBytes * (y - di.bounds[1]) + (size_t)(x1 - di.bounds[0]) * bpp;
            if (x2 > x1) std::memcpy(d, s, (size_t)(x2 - x1) * bpp);
        }
        logf("  done f=%lld lost=%zu distinctFrames=%zu", (long long)f, todo.size(), frames.size());
    }

    gEffect->clipReleaseImage(srcImg);
    gEffect->clipReleaseImage(dstImg);
    return status;
}

// ------------------------------------------------------------------ entry points
static OfxStatus onLoad()
{
    if (!gHost) return kOfxStatErrMissingHostFeature;
    gProp = (const OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gEffect = (const OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gParam = (const OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    char path[1024];
    if (!gLog && GetEnvironmentVariableA("DVCORRUPT_LOG", path, sizeof path) > 0) gLog = fopen(path, "w");
    logf("load: prop=%p effect=%p param=%p", (void*)gProp, (void*)gEffect, (void*)gParam);
    return (gProp && gEffect && gParam) ? kOfxStatOK : kOfxStatErrMissingHostFeature;
}

static OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
    try
    {
        OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
        if (!std::strcmp(action, kOfxActionLoad)) return onLoad();
        if (!std::strcmp(action, kOfxActionUnload)) return kOfxStatOK;
        if (!std::strcmp(action, kOfxActionDescribe)) return describe(effect);
        if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect, inArgs);
        if (!std::strcmp(action, kOfxActionCreateInstance)) return createInstance(effect);
        if (!std::strcmp(action, kOfxActionDestroyInstance)) return destroyInstance(effect);
        if (!std::strcmp(action, kOfxActionInstanceChanged)) return instanceChanged(effect, inArgs);
        if (!std::strcmp(action, kOfxImageEffectActionGetFramesNeeded)) return getFramesNeeded(effect, inArgs, outArgs);
        if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, inArgs);
        return kOfxStatReplyDefault;
    }
    catch (const std::bad_alloc&)
    {
        return kOfxStatErrMemory;
    }
    catch (...)
    {
        return kOfxStatFailed;
    }
}

static void setHost(OfxHost* host) { gHost = host; }

static OfxPlugin gPlugin = {kOfxImageEffectPluginApi, 1, PLUGIN_ID, 1, 0, setHost, mainEntry};

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin* OfxGetPlugin(int nth) { return nth == 0 ? &gPlugin : nullptr; }
