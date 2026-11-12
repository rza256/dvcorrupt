using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using DvCorrupt.Core;
using DvCorrupt.Media;

namespace DvCorrupt;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        var args = e.Args;
        if (args.Length >= 1 && args[0] == "--render")
        {
            // --render <file> <frame> <preset|default> <out.png> [map] [mapping=N] [outsize=WxH]
            int code = 0;
            try { HeadlessRender(args); }
            catch (Exception ex) { File.WriteAllText(args.Length > 4 ? args[4] + ".err.txt" : "render.err.txt", ex.ToString()); code = 1; }
            Shutdown(code);
            return;
        }
        if (args.Length >= 1 && args[0] == "--export")
        {
            // --export <file> <in> <out> <preset> <outpath> <codecIndex>
            int code = 0;
            try
            {
                var info = MediaInfo.Probe(args[1]);
                var sz = SourceSpec.OutputSizes(info)[0];
                var p = new GlitchParams();
                foreach (var pr in GlitchParams.Presets)
                    if (pr.Name.Equals(args[4], StringComparison.OrdinalIgnoreCase)) pr.Apply(p);
                var job = new ExportJob
                {
                    InFrame = long.Parse(args[2]), OutFrame = long.Parse(args[3]), OutputPath = args[5],
                    Codec = ExportJob.Codecs[int.Parse(args[6])].Codec,
                };
                Exporter.Run(new SourceSpec(info, sz.W, sz.H, 0, false), p, job, (_, _) => { }, CancellationToken.None);
            }
            catch (Exception ex) { File.WriteAllText(args[5] + ".err.txt", ex.ToString()); code = 1; }
            Shutdown(code);
            return;
        }
        var w = new MainWindow();
        MainWindow = w;
        w.Show();
        if (args.Length >= 1 && File.Exists(args[0])) w.OpenFile(args[0]);
    }

    static void HeadlessRender(string[] a)
    {
        string file = a[1];
        long frame = long.Parse(a[2]);
        string preset = a[3];
        string outPng = a[4];
        var info = MediaInfo.Probe(file);
        var size = SourceSpec.OutputSizes(info)[0];
        var p = new GlitchParams();
        foreach (var pr in GlitchParams.Presets)
            if (pr.Name.Equals(preset, StringComparison.OrdinalIgnoreCase)) pr.Apply(p);
        foreach (var extra in a.Skip(5))
        {
            if (extra == "map") p.ShowMap = true;
            else if (extra.StartsWith("mapping=")) p.Mapping = int.Parse(extra[8..]);
            else if (extra.StartsWith("mode=")) p.Mode = int.Parse(extra[5..]);
            else if (extra.StartsWith("outsize="))
            {
                var wh = extra[8..].Split('x');
                size = ("", int.Parse(wh[0]), int.Parse(wh[1]));
            }
        }
        var spec = new SourceSpec(info, size.W, size.H, 0, false);
        var layout = new DvLayout(info.IsPal, spec.OutW, spec.OutH);
        int cov = layout.CoverageErrors();

        int step = spec.TapeStep;
        long start = Math.Max(0, frame - 24 * step);
        using var dec = new FfmpegProcess(spec.ReaderArgs(start), true, false);
        var engine = new GlitchEngine(layout, info.Fps, step);
        var scope = new ScopeRenderer();
        int fb = spec.FrameBytes;
        var q = new List<byte[]>();
        void Fill() { while (q.Count <= step) { var b = new byte[fb]; if (!dec.ReadExact(b, fb)) break; q.Add(b); } }
        Fill();
        for (long f = start; f <= frame && q.Count > 0; f++)
        {
            engine.Process(q[0], q.Count > step ? q[step] : null, f, p);
            q.RemoveAt(0);
            Fill();
        }
        scope.Render(engine.Errors);
        Save(outPng, engine.Display, spec.OutW, spec.OutH);
        Save(System.IO.Path.ChangeExtension(outPng, ".scope.png"), scope.Buf, ScopeRenderer.W, ScopeRenderer.H);
        File.WriteAllText(System.IO.Path.ChangeExtension(outPng, ".txt"),
            $"coverageErrors={cov} videoErrors={engine.Errors.VideoErrorCount} audioErrors={engine.Errors.AudioErrorCount} " +
            $"gainA={engine.Errors.HeadGain[0]:0.00} gainB={engine.Errors.HeadGain[1]:0.00} size={spec.OutW}x{spec.OutH} step={step} filter={spec.BuildFilter()}");
    }

    static void Save(string path, byte[] bgr, int w, int h)
    {
        var bmp = BitmapSource.Create(w, h, 96, 96, PixelFormats.Bgr24, null, bgr, w * 3);
        var enc = new PngBitmapEncoder();
        enc.Frames.Add(BitmapFrame.Create(bmp));
        using var fs = File.Create(path);
        enc.Save(fs);
    }
}
