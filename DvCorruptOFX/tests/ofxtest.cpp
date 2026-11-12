// Minimal OpenFX host used to test DvCorrupt.ofx outside VEGAS, and to generate the
// VEGAS preset package from the engine's preset table.
//
//   ofxtest presets <out.xml>
//   ofxtest render <plugin.ofx> <video> <startFrame> <count> <preset> <out_prefix> [float] [frameRate=N]
//
// render: decodes frames with ffmpeg, renders them through the plugin sequentially, then
// renders a cold random-access frame (seek), writes PNG/MP4 via ffmpeg and a stats file.
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include "../src/DvEngine.h"

// ------------------------------------------------------------------ property sets
struct Val { std::string s; double d = 0; int i = 0; void* p = nullptr; };
struct PropSet { std::map<std::string, std::vector<Val>> m; std::string tag; };

static PropSet* PS(OfxPropertySetHandle h) { return (PropSet*)h; }
static Val& slot(OfxPropertySetHandle h, const char* n, int idx)
{
    auto& v = PS(h)->m[n];
    if ((int)v.size() <= idx) v.resize(idx + 1);
    return v[idx];
}
static bool has(OfxPropertySetHandle h, const char* n, int idx)
{
    auto it = PS(h)->m.find(n);
    return it != PS(h)->m.end() && (int)it->second.size() > idx;
}

static OfxStatus pSetPointer(OfxPropertySetHandle h, const char* n, int i, void* v) { slot(h, n, i).p = v; return kOfxStatOK; }
static OfxStatus pSetString(OfxPropertySetHandle h, const char* n, int i, const char* v) { slot(h, n, i).s = v ? v : ""; return kOfxStatOK; }
static OfxStatus pSetDouble(OfxPropertySetHandle h, const char* n, int i, double v) { slot(h, n, i).d = v; slot(h, n, i).i = (int)v; return kOfxStatOK; }
static OfxStatus pSetInt(OfxPropertySetHandle h, const char* n, int i, int v) { slot(h, n, i).i = v; slot(h, n, i).d = v; return kOfxStatOK; }
static OfxStatus pSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v) { for (int i = 0; i < c; i++) pSetPointer(h, n, i, v[i]); return kOfxStatOK; }
static OfxStatus pSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) { for (int i = 0; i < c; i++) pSetString(h, n, i, v[i]); return kOfxStatOK; }
static OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v) { for (int i = 0; i < c; i++) pSetDouble(h, n, i, v[i]); return kOfxStatOK; }
static OfxStatus pSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) { for (int i = 0; i < c; i++) pSetInt(h, n, i, v[i]); return kOfxStatOK; }
static OfxStatus pGetPointer(OfxPropertySetHandle h, const char* n, int i, void** v) { if (!has(h, n, i)) return kOfxStatErrUnknown; *v = slot(h, n, i).p; return kOfxStatOK; }
static OfxStatus pGetString(OfxPropertySetHandle h, const char* n, int i, char** v) { if (!has(h, n, i)) return kOfxStatErrUnknown; *v = (char*)slot(h, n, i).s.c_str(); return kOfxStatOK; }
static OfxStatus pGetDouble(OfxPropertySetHandle h, const char* n, int i, double* v) { if (!has(h, n, i)) return kOfxStatErrUnknown; *v = slot(h, n, i).d; return kOfxStatOK; }
static OfxStatus pGetInt(OfxPropertySetHandle h, const char* n, int i, int* v) { if (!has(h, n, i)) return kOfxStatErrUnknown; *v = slot(h, n, i).i; return kOfxStatOK; }
static OfxStatus pGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v) { for (int i = 0; i < c; i++) if (pGetPointer(h, n, i, &v[i])) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pGetStringN(OfxPropertySetHandle h, const char* n, int c, char** v) { for (int i = 0; i < c; i++) if (pGetString(h, n, i, &v[i])) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v) { for (int i = 0; i < c; i++) if (pGetDouble(h, n, i, &v[i])) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v) { for (int i = 0; i < c; i++) if (pGetInt(h, n, i, &v[i])) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pReset(OfxPropertySetHandle h, const char* n) { PS(h)->m.erase(n); return kOfxStatOK; }
static OfxStatus pGetDim(OfxPropertySetHandle h, const char* n, int* c) { auto it = PS(h)->m.find(n); *c = it == PS(h)->m.end() ? 0 : (int)it->second.size(); return kOfxStatOK; }

// ------------------------------------------------------------------ params / clips / effect
struct Param { std::string type, name; PropSet props; double value = 0; };
struct ParamSet { std::vector<std::unique_ptr<Param>> params; Param* find(const char* n) { for (auto& p : params) if (p->name == n) return p.get(); return nullptr; } };
struct Image { PropSet props; std::vector<uint8_t> data; };

struct Effect;
struct Clip { std::string name; PropSet props; Effect* owner = nullptr; };

struct Effect
{
    PropSet props;
    ParamSet params;
    std::vector<std::unique_ptr<Clip>> clips;
    Clip* clip(const char* n) { for (auto& c : clips) if (c->name == n) return c.get(); return nullptr; }
};

// frames provided to the plugin (bottom-up RGBA8, or float)
static std::map<long long, std::vector<uint8_t>> gFrames;
static int gW = 0, gH = 0;
static bool gFloat = false;
static int gFetches = 0;
static double gFps = 30000.0 / 1001.0;

static OfxStatus paramDefine(OfxParamSetHandle ps, const char* type, const char* name, OfxPropertySetHandle* props)
{
    auto* set = (ParamSet*)ps;
    auto p = std::make_unique<Param>();
    p->type = type; p->name = name;
    if (props) *props = (OfxPropertySetHandle)&p->props;
    set->params.push_back(std::move(p));
    return kOfxStatOK;
}
static OfxStatus paramGetHandle(OfxParamSetHandle ps, const char* name, OfxParamHandle* h, OfxPropertySetHandle* props)
{
    Param* p = ((ParamSet*)ps)->find(name);
    if (!p) return kOfxStatErrUnknown;
    *h = (OfxParamHandle)p;
    if (props) *props = (OfxPropertySetHandle)&p->props;
    return kOfxStatOK;
}
static OfxStatus paramGetPropertySet(OfxParamHandle h, OfxPropertySetHandle* props) { *props = (OfxPropertySetHandle) & ((Param*)h)->props; return kOfxStatOK; }
static OfxStatus paramSetGetPropertySet(OfxParamSetHandle, OfxPropertySetHandle* props) { *props = nullptr; return kOfxStatErrUnsupported; }
static OfxStatus getValue(Param* p, va_list ap)
{
    if (p->type == kOfxParamTypeDouble) *va_arg(ap, double*) = p->value;
    else *va_arg(ap, int*) = (int)p->value;
    return kOfxStatOK;
}
static OfxStatus paramGetValue(OfxParamHandle h, ...) { va_list ap; va_start(ap, h); auto r = getValue((Param*)h, ap); va_end(ap); return r; }
static OfxStatus paramGetValueAtTime2(OfxParamHandle h, OfxTime t, ...)
{
    va_list ap;
    va_start(ap, t);
    auto r = getValue((Param*)h, ap);
    va_end(ap);
    return r;
}

static OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* props) { *props = (OfxPropertySetHandle) & ((Effect*)e)->props; return kOfxStatOK; }
static OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle* ps) { *ps = (OfxParamSetHandle) & ((Effect*)e)->params; return kOfxStatOK; }
static OfxStatus clipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* props)
{
    auto c = std::make_unique<Clip>();
    c->name = name; c->owner = (Effect*)e;
    *props = (OfxPropertySetHandle)&c->props;
    ((Effect*)e)->clips.push_back(std::move(c));
    return kOfxStatOK;
}
static OfxStatus clipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* c, OfxPropertySetHandle* props)
{
    Clip* cl = ((Effect*)e)->clip(name);
    if (!cl) return kOfxStatErrUnknown;
    *c = (OfxImageClipHandle)cl;
    if (props) *props = (OfxPropertySetHandle)&cl->props;
    return kOfxStatOK;
}
static OfxStatus clipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle* props) { *props = (OfxPropertySetHandle) & ((Clip*)c)->props; return kOfxStatOK; }

static Image* makeImage(int w, int h)
{
    auto* img = new Image();
    int bpp = gFloat ? 16 : 4;
    img->data.assign((size_t)w * h * bpp, 0);
    OfxPropertySetHandle ph = (OfxPropertySetHandle)&img->props;
    pSetPointer(ph, kOfxImagePropData, 0, img->data.data());
    int b[4] = {0, 0, w, h};
    pSetIntN(ph, kOfxImagePropBounds, 4, b);
    pSetIntN(ph, kOfxImagePropRegionOfDefinition, 4, b);
    pSetInt(ph, kOfxImagePropRowBytes, 0, w * bpp);
    pSetString(ph, kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
    pSetString(ph, kOfxImageEffectPropPixelDepth, 0, gFloat ? kOfxBitDepthFloat : kOfxBitDepthByte);
    pSetString(ph, kOfxImageEffectPropPreMultiplication, 0, kOfxImageOpaque);
    pSetDouble(ph, kOfxImagePropPixelAspectRatio, 0, 1.0);
    double rs[2] = {1, 1};
    pSetDoubleN(ph, kOfxImageEffectPropRenderScale, 2, rs);
    pSetString(ph, kOfxImagePropField, 0, kOfxImageFieldNone);
    return img;
}

static OfxStatus clipGetImage(OfxImageClipHandle c, OfxTime t, const OfxRectD*, OfxPropertySetHandle* out)
{
    Clip* cl = (Clip*)c;
    Image* img = makeImage(gW, gH);
    if (cl->name == kOfxImageEffectSimpleSourceClipName)
    {
        auto it = gFrames.find((long long)floor(t + 1e-3));
        if (it == gFrames.end()) { delete img; return kOfxStatFailed; }
        gFetches++;
        if (gFloat)
        {
            float* d = (float*)img->data.data();
            for (size_t i = 0; i < it->second.size(); i++) d[i] = it->second[i] / 255.f;
        }
        else img->data = it->second;
        pSetPointer((OfxPropertySetHandle)&img->props, kOfxImagePropData, 0, img->data.data());
    }
    *out = (OfxPropertySetHandle)&img->props;
    return kOfxStatOK;
}
static OfxStatus clipReleaseImage(OfxPropertySetHandle h)
{
    // PropSet is the first member of Image
    delete (Image*)h;
    return kOfxStatOK;
}
static OfxStatus clipGetRoD(OfxImageClipHandle, OfxTime, OfxRectD* r) { r->x1 = 0; r->y1 = 0; r->x2 = gW; r->y2 = gH; return kOfxStatOK; }
static int abortFn(OfxImageEffectHandle) { return 0; }

static OfxPropertySuiteV1 gPropSuite;
static OfxParameterSuiteV1 gParamSuite;
static OfxImageEffectSuiteV1 gEffectSuite;
static PropSet gHostProps;

static const void* fetchSuite(OfxPropertySetHandle, const char* name, int version)
{
    if (!strcmp(name, kOfxPropertySuite) && version == 1) return &gPropSuite;
    if (!strcmp(name, kOfxParameterSuite) && version == 1) return &gParamSuite;
    if (!strcmp(name, kOfxImageEffectSuite) && version == 1) return &gEffectSuite;
    return nullptr;
}

static void initSuites()
{
    gPropSuite = {};
    gPropSuite.propSetPointer = pSetPointer; gPropSuite.propSetString = pSetString; gPropSuite.propSetDouble = pSetDouble; gPropSuite.propSetInt = pSetInt;
    gPropSuite.propSetPointerN = pSetPointerN; gPropSuite.propSetStringN = pSetStringN; gPropSuite.propSetDoubleN = pSetDoubleN; gPropSuite.propSetIntN = pSetIntN;
    gPropSuite.propGetPointer = pGetPointer; gPropSuite.propGetString = pGetString; gPropSuite.propGetDouble = pGetDouble; gPropSuite.propGetInt = pGetInt;
    gPropSuite.propGetPointerN = pGetPointerN; gPropSuite.propGetStringN = pGetStringN; gPropSuite.propGetDoubleN = pGetDoubleN; gPropSuite.propGetIntN = pGetIntN;
    gPropSuite.propReset = pReset; gPropSuite.propGetDimension = pGetDim;

    gParamSuite = {};
    gParamSuite.paramDefine = paramDefine; gParamSuite.paramGetHandle = paramGetHandle;
    gParamSuite.paramSetGetPropertySet = paramSetGetPropertySet; gParamSuite.paramGetPropertySet = paramGetPropertySet;
    gParamSuite.paramGetValue = paramGetValue; gParamSuite.paramGetValueAtTime = paramGetValueAtTime2;

    gEffectSuite = {};
    gEffectSuite.getPropertySet = getPropertySet; gEffectSuite.getParamSet = getParamSet;
    gEffectSuite.clipDefine = clipDefine; gEffectSuite.clipGetHandle = clipGetHandle; gEffectSuite.clipGetPropertySet = clipGetPropertySet;
    gEffectSuite.clipGetImage = clipGetImage; gEffectSuite.clipReleaseImage = clipReleaseImage;
    gEffectSuite.clipGetRegionOfDefinition = clipGetRoD; gEffectSuite.abort = abortFn;
    pSetString((OfxPropertySetHandle)&gHostProps, kOfxPropName, 0, "dvcorrupt.ofxtest");
}

// ------------------------------------------------------------------ preset package
static std::string fmt(double v)
{
    char b[64];
    snprintf(b, sizeof b, "%.6f", v);
    return b;
}

static int writePresets(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 1;
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<OfxPresetPackage>\n");
    for (auto& pr : dvc::presets())
    {
        dvc::Params p;
        pr.apply(p);
        fprintf(f, "<OfxPreset plugin=\"com.dvcorrupt:DVCorrupt\" context=\"Filter\" name=\"%s\">\n", pr.name);
        fprintf(f, "\t<OfxPlugin>com.dvcorrupt:DVCorrupt</OfxPlugin>\n\t<OfxPluginVersion>1 0</OfxPluginVersion>\n");
        for (auto& d : dvc::paramDefs())
        {
            double v = d.get(p);
            switch (d.kind)
            {
            case dvc::PKind::Double:
                fprintf(f, "\t<OfxParamTypeDouble name=\"%s\">\n\t\t<OfxParamValue>%s</OfxParamValue>\n\t</OfxParamTypeDouble>\n", d.name, fmt(v).c_str());
                break;
            case dvc::PKind::Int:
                fprintf(f, "\t<OfxParamTypeInteger name=\"%s\">\n\t\t<OfxParamValue>%d</OfxParamValue>\n\t</OfxParamTypeInteger>\n", d.name, (int)v);
                break;
            case dvc::PKind::Choice:
                fprintf(f, "\t<OfxParamTypeChoice name=\"%s\">\n\t\t<OfxParamValue>%d</OfxParamValue>\n\t</OfxParamTypeChoice>\n", d.name, (int)v);
                break;
            case dvc::PKind::Bool:
                fprintf(f, "\t<OfxParamTypeBoolean name=\"%s\">\n\t\t<OfxParamValue>%s</OfxParamValue>\n\t</OfxParamTypeBoolean>\n", d.name, v > 0.5 ? "true" : "false");
                break;
            }
        }
        fprintf(f, "</OfxPreset>\n");
    }
    fprintf(f, "</OfxPresetPackage>\n");
    fclose(f);
    return 0;
}

// ------------------------------------------------------------------ ffmpeg helpers
static bool decodeFrames(const char* video, long long first, int count)
{
    char cmd[2048];
    double t = first * 1001.0 / 30000.0;
    if (gFps > 0) t = first / gFps;
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -nostdin -ss %.6f -i \"%s\" -map 0:v:0 -frames:v %d "
             "-vf \"crop='min(iw,ih*4/3/sar)':ih,scale=720:480:in_color_matrix=auto,vflip\" -f rawvideo -pix_fmt rgba -",
             t, video, count);
    FILE* p = _popen(cmd, "rb");
    if (!p) return false;
    gW = 720; gH = 480;
    size_t fb = (size_t)gW * gH * 4;
    for (int i = 0; i < count; i++)
    {
        std::vector<uint8_t> buf(fb);
        if (fread(buf.data(), 1, fb, p) != fb) break;
        gFrames[first + i] = std::move(buf);
    }
    _pclose(p);
    return !gFrames.empty();
}

int main(int argc, char** argv)
{
    if (argc >= 3 && !strcmp(argv[1], "presets")) return writePresets(argv[2]);
    if (argc < 8 || strcmp(argv[1], "render")) { fprintf(stderr, "usage: see source\n"); return 2; }
    const char* pluginPath = argv[2];
    const char* video = argv[3];
    long long start = atoll(argv[4]);
    int count = atoi(argv[5]);
    std::string preset = argv[6];
    std::string outPrefix = argv[7];
    for (int i = 8; i < argc; i++)
    {
        if (!strcmp(argv[i], "float")) gFloat = true;
        if (!strncmp(argv[i], "frameRate=", 10)) gFps = atof(argv[i] + 10);
    }

    // layout self-check
    {
        dvc::Layout L;
        L.build(false, 720, 480);
        int c1 = L.coverageErrors();
        L.build(true, 720, 576);
        int c2 = L.coverageErrors();
        printf("layout coverage errors: ntsc=%d pal=%d\n", c1, c2);
    }

    const int pre = 40;
    long long first = std::max(0LL, start - pre);
    if (!decodeFrames(video, first, (int)(start - first) + count + 4)) { fprintf(stderr, "decode failed\n"); return 3; }
    printf("decoded %zu frames %dx%d\n", gFrames.size(), gW, gH);

    initSuites();
    HMODULE mod = LoadLibraryA(pluginPath);
    if (!mod) { fprintf(stderr, "LoadLibrary failed %lu\n", GetLastError()); return 4; }
    auto getN = (int (*)())GetProcAddress(mod, "OfxGetNumberOfPlugins");
    auto getP = (OfxPlugin * (*)(int)) GetProcAddress(mod, "OfxGetPlugin");
    if (!getN || !getP) { fprintf(stderr, "missing exports\n"); return 5; }
    printf("plugins: %d\n", getN());
    OfxPlugin* pl = getP(0);
    printf("id=%s api=%s v%u.%u\n", pl->pluginIdentifier, pl->pluginApi, pl->pluginVersionMajor, pl->pluginVersionMinor);
    static OfxHost host = {(OfxPropertySetHandle)&gHostProps, fetchSuite};
    pl->setHost(&host);
    auto act = [&](const char* a, void* h, PropSet* in, PropSet* out) {
        OfxStatus s = pl->mainEntry(a, h, (OfxPropertySetHandle)in, (OfxPropertySetHandle)out);
        return s;
    };
    if (act(kOfxActionLoad, nullptr, nullptr, nullptr) != kOfxStatOK) { fprintf(stderr, "load failed\n"); return 6; }

    Effect desc;
    if (act(kOfxActionDescribe, &desc, nullptr, nullptr) != kOfxStatOK) { fprintf(stderr, "describe failed\n"); return 7; }
    PropSet ctxArgs;
    pSetString((OfxPropertySetHandle)&ctxArgs, kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    if (act(kOfxImageEffectActionDescribeInContext, &desc, &ctxArgs, nullptr) != kOfxStatOK) { fprintf(stderr, "describeInContext failed\n"); return 8; }
    printf("label=%s  params=%zu clips=%zu\n", desc.props.m[kOfxPropLabel][0].s.c_str(), desc.params.params.size(), desc.clips.size());

    // instance: copy descriptors, defaults -> values
    Effect inst;
    inst.props = desc.props;
    pSetString((OfxPropertySetHandle)&inst.props, kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    pSetDouble((OfxPropertySetHandle)&inst.props, kOfxImageEffectPropFrameRate, 0, gFps);
    for (auto& p : desc.params.params)
    {
        auto q = std::make_unique<Param>(*p);
        auto it = q->props.m.find(kOfxParamPropDefault);
        if (it != q->props.m.end() && !it->second.empty()) q->value = q->type == kOfxParamTypeDouble ? it->second[0].d : it->second[0].i;
        inst.params.params.push_back(std::move(q));
    }
    for (auto& c : desc.clips)
    {
        auto q = std::make_unique<Clip>(*c);
        q->owner = &inst;
        pSetDouble((OfxPropertySetHandle)&q->props, kOfxImageEffectPropFrameRate, 0, gFps);
        double range[2] = {0, 1e6};
        pSetDoubleN((OfxPropertySetHandle)&q->props, kOfxImageEffectPropFrameRange, 2, range);
        inst.clips.push_back(std::move(q));
    }
    if (act(kOfxActionCreateInstance, &inst, nullptr, nullptr) != kOfxStatOK) { fprintf(stderr, "createInstance failed\n"); return 9; }

    // apply preset by name
    dvc::Params pv;
    bool found = false;
    for (auto& pr : dvc::presets()) if (preset == pr.name) { pr.apply(pv); found = true; }
    if (!found) printf("preset '%s' not found; using defaults\n", preset.c_str());
    for (auto& d : dvc::paramDefs())
        if (Param* p = inst.params.find(d.name)) p->value = d.get(pv);
    if (Param* sc = inst.params.find("ShowScope")) sc->value = 0;
    PropSet chg;
    pSetString((OfxPropertySetHandle)&chg, kOfxPropType, 0, kOfxTypeParameter);
    pSetString((OfxPropertySetHandle)&chg, kOfxPropChangeReason, 0, kOfxChangePluginEdited);
    act(kOfxActionInstanceChanged, &inst, &chg, nullptr);

    // frames-needed check
    {
        PropSet in, out;
        pSetDouble((OfxPropertySetHandle)&in, kOfxPropTime, 0, (double)start);
        act(kOfxImageEffectActionGetFramesNeeded, &inst, &in, &out);
        auto& r = out.m["OfxImageClipPropFrameRange_Source"];
        if (r.size() == 2) printf("frames needed at %lld: [%g, %g]\n", start, r[0].d, r[1].d);
    }

    // render sequence and stream to ffmpeg
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -y -f rawvideo -pix_fmt rgba -s %dx%d -framerate %g -i - -vf vflip -c:v libx264 -crf 16 -pix_fmt yuv420p \"%s.mp4\"",
             gW, gH, gFps, outPrefix.c_str());
    FILE* enc = _popen(cmd, "wb");
    std::vector<uint8_t> firstOut, rgba8((size_t)gW * gH * 4);
    double totalMs = 0, maxMs = 0, firstMs = 0;
    int fetchesFirst = 0, fetchesSeq = 0;
    auto renderAt = [&](double t, std::vector<uint8_t>& outRgba, double& ms, int& fetches) -> OfxStatus {
        PropSet in;
        OfxPropertySetHandle ih = (OfxPropertySetHandle)&in;
        pSetDouble(ih, kOfxPropTime, 0, t);
        int rw[4] = {0, 0, gW, gH};
        pSetIntN(ih, kOfxImageEffectPropRenderWindow, 4, rw);
        double rs[2] = {1, 1};
        pSetDoubleN(ih, kOfxImageEffectPropRenderScale, 2, rs);
        pSetString(ih, kOfxImageEffectPropFieldToRender, 0, kOfxImageFieldNone);
        // capture the output image: wrap clipGetImage for dst by temporarily remembering the pointer
        static Image* lastDst;
        lastDst = nullptr;
        gEffectSuite.clipGetImage = [](OfxImageClipHandle c, OfxTime tt, const OfxRectD* r, OfxPropertySetHandle* o) -> OfxStatus {
            OfxStatus s = clipGetImage(c, tt, r, o);
            if (s == kOfxStatOK && ((Clip*)c)->name == kOfxImageEffectOutputClipName) lastDst = (Image*)*o;
            return s;
        };
        gEffectSuite.clipReleaseImage = [](OfxPropertySetHandle h) -> OfxStatus {
            if ((Image*)h == lastDst) return kOfxStatOK;   // keep until we copy it
            return clipReleaseImage(h);
        };
        int f0 = gFetches;
        auto t0 = std::chrono::high_resolution_clock::now();
        OfxStatus s = act(kOfxImageEffectActionRender, &inst, &in, nullptr);
        ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        fetches = gFetches - f0;
        if (lastDst)
        {
            if (gFloat)
            {
                const float* d = (const float*)lastDst->data.data();
                for (size_t i = 0; i < outRgba.size(); i++) outRgba[i] = (uint8_t)std::min(255.f, std::max(0.f, d[i] * 255.f + 0.5f));
            }
            else outRgba = lastDst->data;
            delete lastDst;
        }
        return s;
    };

    int failures = 0;
    std::vector<std::vector<uint8_t>> seqOut;
    for (int i = 0; i < count; i++)
    {
        double ms; int fe;
        if (renderAt(start + i, rgba8, ms, fe) != kOfxStatOK) failures++;
        if (i == 0) { firstMs = ms; fetchesFirst = fe; firstOut = rgba8; }
        else { totalMs += ms; maxMs = std::max(maxMs, ms); fetchesSeq += fe; }
        fwrite(rgba8.data(), 1, rgba8.size(), enc);
        seqOut.push_back(rgba8);
    }
    _pclose(enc);

    // order independence: reverse order, and field-pair style calls at t and t+0.5, after a cache reset
    {
        act(kOfxActionInstanceChanged, &inst, &chg, nullptr);
        int mismatchRev = 0, mismatchField = 0;
        std::vector<uint8_t> tmp((size_t)gW * gH * 4);
        for (int i = count - 1; i >= 0; i--)
        {
            double ms; int fe;
            renderAt(start + i, tmp, ms, fe);
            if (tmp != seqOut[i]) mismatchRev++;
        }
        act(kOfxActionInstanceChanged, &inst, &chg, nullptr);
        for (int i = 0; i < count; i += 3)
        {
            double ms; int fe;
            // field-pass style: second field at t+0.5 (rendered first), then t; both must equal frame t
            renderAt(start + i + 0.5, tmp, ms, fe);
            if (tmp != seqOut[i]) mismatchField++;
            renderAt(start + i, tmp, ms, fe);
            if (tmp != seqOut[i]) mismatchField++;
        }
        printf("order independence: reverse-order mismatches=%d, field/stride mismatches=%d\n", mismatchRev, mismatchField);
    }

    // cold seek: brand-new param generation then render frame start+count/2 directly
    act(kOfxActionInstanceChanged, &inst, &chg, nullptr);
    long long seekT = start + count / 2;
    double seekMs; int seekFetch;
    std::vector<uint8_t> seekOut((size_t)gW * gH * 4);
    renderAt(seekT, seekOut, seekMs, seekFetch);

    auto writePng = [&](const std::vector<uint8_t>& px, const std::string& path) {
        char c[2048];
        snprintf(c, sizeof c, "ffmpeg -v error -y -f rawvideo -pix_fmt rgba -s %dx%d -i - -vf vflip -frames:v 1 \"%s\"", gW, gH, path.c_str());
        FILE* f = _popen(c, "wb");
        fwrite(px.data(), 1, px.size(), f);
        _pclose(f);
    };
    writePng(firstOut, outPrefix + "_first.png");
    writePng(rgba8, outPrefix + "_last.png");
    writePng(seekOut, outPrefix + "_seek.png");

    printf("render failures: %d\n", failures);
    printf("first frame (cold, warm-up): %.1f ms, %d source fetches\n", firstMs, fetchesFirst);
    printf("sequential: avg %.2f ms, max %.2f ms, %.2f fetches/frame\n", totalMs / std::max(1, count - 1), maxMs, fetchesSeq / (double)std::max(1, count - 1));
    printf("cold seek to %lld: %.1f ms, %d fetches\n", seekT, seekMs, seekFetch);

    act(kOfxActionDestroyInstance, &inst, nullptr, nullptr);
    act(kOfxActionUnload, nullptr, nullptr, nullptr);
    return failures ? 10 : 0;
}
