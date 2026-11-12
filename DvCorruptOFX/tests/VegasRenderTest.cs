// VEGAS Pro script: end-to-end test of the DV Corrupt OFX plugin inside VEGAS.
// Run:  vegas210.exe -SCRIPT:"...\VegasRenderTest.cs"
// Writes a log next to this script (VegasRenderTest.log) and renders test files to the build folder.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using ScriptPortal.Vegas;

public class EntryPoint
{
    const string Root = @"C:\Users\skul\Downloads\dvcorrupt";
    readonly string logPath = Path.Combine(Root, @"DvCorruptOFX\build\VegasRenderTest.log");
    readonly StringBuilder log = new StringBuilder();

    void L(string s)
    {
        log.AppendLine(DateTime.Now.ToString("HH:mm:ss.fff") + "  " + s);
        try { File.WriteAllText(logPath, log.ToString()); } catch { }
    }

    static PlugInNode Find(PlugInNode node, Func<PlugInNode, bool> pred)
    {
        foreach (PlugInNode c in node)
        {
            if (!c.IsContainer && pred(c)) return c;
            if (c.IsContainer)
            {
                var r = Find(c, pred);
                if (r != null) return r;
            }
        }
        return null;
    }

    static void Collect(PlugInNode node, List<PlugInNode> list)
    {
        foreach (PlugInNode c in node)
        {
            if (c.IsContainer) Collect(c, list);
            else list.Add(c);
        }
    }

    public void FromVegas(Vegas vegas)
    {
        try
        {
            L("VEGAS " + vegas.Version);
            PlugInNode fx = Find(vegas.VideoFX, n => n.Name.IndexOf("DV Corrupt", StringComparison.OrdinalIgnoreCase) >= 0);
            if (fx == null)
            {
                L("DV Corrupt NOT FOUND. OFX plug-ins seen:");
                var all = new List<PlugInNode>();
                Collect(vegas.VideoFX, all);
                foreach (var n in all) if (n.IsOFX) L("  " + n.Name + "  " + n.UniqueID);
                return;
            }
            L("FOUND plug-in: '" + fx.Name + "' uid=" + fx.UniqueID + " ofx=" + fx.IsOFX);

            vegas.NewProject(false, false);
            Project p = vegas.Project;
            p.Video.Width = 720;
            p.Video.Height = 480;
            p.Video.FrameRate = 30000.0 / 1001.0;
            p.Video.PixelAspectRatio = 0.9091;
            try { p.Video.FieldOrder = (VideoFieldOrder)Enum.Parse(typeof(VideoFieldOrder), "LowerFieldFirst"); } catch (Exception ex) { L("field order: " + ex.Message); }
            try { p.Video.RenderQuality = (VideoRenderQuality)Enum.Parse(typeof(VideoRenderQuality), "Best"); } catch { }
            L("PixelFormat values: " + string.Join(", ", Enum.GetNames(typeof(PixelFormat))));

            Media media = new Media(Path.Combine(Root, "WinDV.06-11-08_14-29.00.avi"));
            VideoTrack track = p.AddVideoTrack();
            VideoEvent ev = track.AddVideoEvent(Timecode.FromFrames(0), Timecode.FromFrames(300));
            Take take = ev.AddTake(media.GetVideoStreamByIndex(0));
            take.Offset = Timecode.FromFrames(2950);

            Effect effect = ev.Effects.AddEffect(fx);

            var presetNames = new List<string>();
            foreach (EffectPreset pr in effect.Presets) presetNames.Add(pr.Name);
            L("presets (" + presetNames.Count + "): " + string.Join(" | ", presetNames));

            OFXEffect ofx = effect.OFXEffect;
            int nParams = 0;
            foreach (OFXParameter op in ofx.Parameters) nParams++;
            L("OFX parameters: " + nParams);

            effect.Preset = "Clogged head A";
            var headA = ofx.FindParameterByName("HeadAClog") as OFXDoubleParameter;
            L("after preset 'Clogged head A': HeadAClog=" + (headA != null ? headA.Value.ToString() : "null"));

            // pick a renderer/template: prefer DV AVI, else first MP4 template
            RenderTemplate tmpl = null; Renderer tmplR = null;
            foreach (Renderer r in vegas.Renderers)
            {
                foreach (RenderTemplate t in r.Templates)
                {
                    if (tmpl == null && r.FileTypeName.IndexOf("Video for Windows", StringComparison.OrdinalIgnoreCase) >= 0 &&
                        t.Name.IndexOf("NTSC DV", StringComparison.OrdinalIgnoreCase) >= 0 && t.IsValid())
                        { tmpl = t; tmplR = r; }
                }
            }
            if (tmpl == null)
                foreach (Renderer r in vegas.Renderers)
                    foreach (RenderTemplate t in r.Templates)
                        if (tmpl == null && t.IsValid() && r.FileExtension.IndexOf("mp4", StringComparison.OrdinalIgnoreCase) >= 0) { tmpl = t; tmplR = r; }
            if (tmpl == null) { L("no render template found"); return; }
            L("template: " + tmplR.FileTypeName + " / " + tmpl.Name);

            string ext = tmplR.FileExtension.Replace("*", "");
            if (!ext.StartsWith(".")) ext = "." + ext;
            string out1 = Path.Combine(Root, @"DvCorruptOFX\build\vegas_clogA" + ext);
            var sw = Stopwatch.StartNew();
            RenderStatus st = vegas.Render(out1, tmpl, Timecode.FromFrames(0), Timecode.FromFrames(300));
            L("render 8-bit 'Clogged head A' -> " + st + " in " + sw.Elapsed.TotalSeconds.ToString("0.0") + " s : " + out1);

            // second pass: 32-bit float project, diagonal guide sweep
            try
            {
                foreach (string n in Enum.GetNames(typeof(PixelFormat)))
                    if (n.IndexOf("32", StringComparison.Ordinal) >= 0 || n.IndexOf("Float", StringComparison.OrdinalIgnoreCase) >= 0)
                    { p.Video.PixelFormat = (PixelFormat)Enum.Parse(typeof(PixelFormat), n); L("pixel format -> " + n); break; }
            }
            catch (Exception ex) { L("pixel format change failed: " + ex.Message); }
            effect.Preset = "Guide sweep (diagonal)";
            string out2 = Path.Combine(Root, @"DvCorruptOFX\build\vegas_sweep_float" + ext);
            sw.Restart();
            st = vegas.Render(out2, tmpl, Timecode.FromFrames(0), Timecode.FromFrames(300));
            L("render float 'Guide sweep (diagonal)' -> " + st + " in " + sw.Elapsed.TotalSeconds.ToString("0.0") + " s : " + out2);
        }
        catch (Exception ex)
        {
            L("EXCEPTION " + ex);
        }
        finally
        {
            L("DONE");
        }
    }
}
