using DvCorrupt.Core;

namespace DvCorrupt.Media;

public enum ExportCodec { DvAvi, ProResMov, H264Mp4, Ffv1Mkv }

public sealed class ExportJob
{
    public string OutputPath = "";
    public ExportCodec Codec;
    public bool IncludeAudio = true;
    public bool GlitchAudio = true;
    /// <summary>For double-rate progressive sources: weave field pairs back into 29.97i / 25i.</summary>
    public bool Reinterlace = true;
    public long InFrame, OutFrame;   // OutFrame exclusive

    public static readonly (string Label, ExportCodec Codec, string Ext)[] Codecs =
    {
        ("DV (dvsd) .avi – SD raster only", ExportCodec.DvAvi, "avi"),
        ("ProRes 422 HQ .mov", ExportCodec.ProResMov, "mov"),
        ("H.264 CRF 14 .mp4", ExportCodec.H264Mp4, "mp4"),
        ("FFV1 lossless .mkv", ExportCodec.Ffv1Mkv, "mkv"),
    };
}

public static class Exporter
{
    const int Preroll = 24;   // tape frames simulated before the in-point

    public static void Run(SourceSpec spec, GlitchParams prm, ExportJob job, Action<double, string> progress, CancellationToken ct)
    {
        var info = spec.Info;
        var p = prm.Clone();
        p.ShowMap = false;
        int W = spec.OutW, H = spec.OutH, fb = spec.FrameBytes;
        double fps = info.Fps;
        int step = spec.TapeStep;

        // Double-rate progressive (deinterlaced 1080i -> 59.94p): optionally weave field pairs back
        // into 29.97i. DV has no 59.94p mode, so DV output always re-interlaces.
        bool reinterlace = step == 2 && (job.Reinterlace || job.Codec == ExportCodec.DvAvi);
        if (step > 1)
        {
            // keep field pairs aligned with tape frames
            job.InFrame -= job.InFrame % step;
            job.OutFrame -= (job.OutFrame - job.InFrame) % step;
            if (job.OutFrame <= job.InFrame) throw new Exception("Export range is too short.");
        }

        if (job.Codec == ExportCodec.DvAvi)
        {
            if (!(spec.IsSdNtscRaster && !info.IsPal) && !(spec.IsSdPalRaster && info.IsPal))
                throw new Exception("DV output needs the 720×480 (NTSC) or 720×576 (PAL) working size. Pick it under Source / output, or choose another codec.");
            double outFps = fps / (reinterlace ? 2 : 1);
            if (Math.Abs(outFps - (info.IsPal ? 25 : 30000.0 / 1001)) > 0.05)
                throw new Exception($"DV output needs {(info.IsPal ? "25" : "29.97")} fps; this source is {fps:0.###} fps.");
        }

        var layout = new DvLayout(info.IsPal, W, H);
        string? wav = null;
        try
        {
            // ---------- audio pass ----------
            if (job.IncludeAudio && info.HasAudio)
            {
                progress(0, "Reading audio…");
                long s0 = AudioGlitch.SampleAt(job.InFrame, info.FpsNum, info.FpsDen);
                long s1 = AudioGlitch.SampleAt(job.OutFrame, info.FpsNum, info.FpsDen);
                string ss = s0 > 0 ? $"-ss {Tools.Num(s0 / (double)AudioGlitch.Rate)} " : "";
                byte[] raw;
                using (var a = new FfmpegProcess(
                           $"-hide_banner -loglevel error -nostdin {ss}-i \"{info.Path}\" -map 0:a:0 -vn " +
                           $"-t {Tools.Num((s1 - s0) / (double)AudioGlitch.Rate)} -ac 2 -ar {AudioGlitch.Rate} -f s16le -",
                           readStdout: true, writeStdin: false))
                {
                    raw = a.ReadAll();
                    a.Finish();
                }
                var pcm = new short[raw.Length / 2 & ~1];
                Buffer.BlockCopy(raw, 0, pcm, 0, pcm.Length * 2);
                if (job.GlitchAudio)
                {
                    // audio errors are per tape frame (tape fps = fps / step)
                    var fe = new FrameErrors(layout.Tracks);
                    int tNum = info.FpsNum, tDen = info.FpsDen * step;
                    for (long tf = job.InFrame / step; tf < job.OutFrame / step; tf++)
                    {
                        ct.ThrowIfCancellationRequested();
                        TapeModel.Compute(tf, p, layout, tNum / (double)tDen, fe);
                        AudioGlitch.ApplyFrame(pcm, s0, tf, tNum, tDen, fe);
                    }
                }
                wav = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"dvcorrupt_{Guid.NewGuid():N}.wav");
                WriteWav(wav, pcm);
            }

            // ---------- video pass ----------
            bool sd = H <= 576;
            string matrix = sd ? "bt601" : "bt709";
            string tags = sd
                ? "-colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m"
                : "-colorspace bt709 -color_primaries bt709 -color_trc bt709";

            var vf = new List<string>();
            string fo;   // field order of the encoded output: "tt", "bb" or ""
            if (reinterlace)
            {
                // frame pair (even, odd) = (first field, second field) of one tape frame
                if (job.Codec == ExportCodec.DvAvi) { vf.Add("tinterlace=mode=interleave_bottom"); fo = "bb"; }
                else { vf.Add("tinterlace=mode=interleave_top"); fo = "tt"; }
                vf.Add(fo == "tt" ? "setfield=tff" : "setfield=bff");
            }
            else
            {
                fo = spec.Interlaced ? info.EffectiveFieldOrder : "";
                vf.Add(fo == "tt" ? "setfield=tff" : fo == "bb" ? "setfield=bff" : "setfield=prog");
                if (job.Codec == ExportCodec.DvAvi && fo == "tt") { vf.Add("fieldorder=bff"); fo = "bb"; }   // DV is bottom-field-first
            }

            string pix, codec, acodec;
            switch (job.Codec)
            {
                case ExportCodec.DvAvi:
                    pix = info.IsPal ? "yuv420p" : "yuv411p";
                    codec = "-c:v dvvideo";
                    acodec = "-c:a pcm_s16le -ar 48000";
                    break;
                case ExportCodec.ProResMov:
                    pix = "yuv422p10le";
                    codec = "-c:v prores_ks -profile:v 3 -vendor apl0";
                    acodec = "-c:a pcm_s16le";
                    break;
                case ExportCodec.H264Mp4:
                    pix = "yuv420p";
                    codec = "-c:v libx264 -preset slow -crf 14" +
                            (fo != "" ? $" -flags +ildct+ilme -x264-params {(fo == "tt" ? "tff" : "bff")}=1" : "");
                    acodec = "-c:a aac -b:a 320k";
                    break;
                default:
                    pix = "yuv422p";
                    codec = "-c:v ffv1 -level 3 -g 1 -slicecrc 1";
                    acodec = "-c:a pcm_s16le";
                    break;
            }
            vf.Add($"scale=out_color_matrix={matrix}:out_range=tv");
            vf.Add($"format={pix}");

            string audioIn = wav != null ? $"-i \"{wav}\" " : "";
            string audioMap = wav != null ? $"-map 1:a:0 {acodec} " : "";
            string encArgs =
                $"-hide_banner -loglevel error -y -f rawvideo -pix_fmt bgr24 -s {W}x{H} -framerate {info.FpsNum}/{info.FpsDen} -i - " +
                audioIn + $"-map 0:v:0 {audioMap}-vf \"{string.Join(",", vf)}\" {codec} -aspect 4:3 {tags} \"{job.OutputPath}\"";

            long start = Math.Max(0, job.InFrame - Preroll * step);
            using var dec = new FfmpegProcess(spec.ReaderArgs(start), readStdout: true, writeStdin: false);
            using var enc = new FfmpegProcess(encArgs, readStdout: false, writeStdin: true);
            var engine = new GlitchEngine(layout, fps, step);

            // look-ahead queue: q[0] = frame f, q[step] = same field of the next tape frame
            var q = new List<byte[]>();
            var spare = new Stack<byte[]>();
            bool eof = false;
            void Fill()
            {
                while (!eof && q.Count <= step)
                {
                    var buf = spare.Count > 0 ? spare.Pop() : new byte[fb];
                    if (dec.ReadExact(buf, fb)) q.Add(buf);
                    else { eof = true; spare.Push(buf); }
                }
            }

            long total = job.OutFrame - job.InFrame;
            Fill();
            for (long f = start; f < job.OutFrame && q.Count > 0; f++)
            {
                ct.ThrowIfCancellationRequested();
                engine.Process(q[0], q.Count > step ? q[step] : null, f, p);
                if (f >= job.InFrame)
                {
                    try { enc.In!.Write(engine.Output, 0, fb); }
                    catch (IOException) { throw new Exception("Encoder stopped: " + enc.ErrorText); }
                    if ((f & 7) == 0) progress((f - job.InFrame) / (double)Math.Max(1, total), $"Frame {f - job.InFrame + 1} / {total}");
                }
                spare.Push(q[0]);
                q.RemoveAt(0);
                Fill();
            }
            int code = enc.Finish();
            if (code != 0) throw new Exception("ffmpeg encode failed: " + enc.ErrorText);
            progress(1, "Done");
        }
        finally
        {
            if (wav != null) try { File.Delete(wav); } catch { }
        }
    }

    static void WriteWav(string path, short[] pcm)
    {
        using var fs = File.Create(path);
        using var w = new BinaryWriter(fs);
        int dataBytes = pcm.Length * 2;
        w.Write("RIFF"u8.ToArray()); w.Write(36 + dataBytes); w.Write("WAVE"u8.ToArray());
        w.Write("fmt "u8.ToArray()); w.Write(16); w.Write((short)1); w.Write((short)2);
        w.Write(AudioGlitch.Rate); w.Write(AudioGlitch.Rate * 4); w.Write((short)4); w.Write((short)16);
        w.Write("data"u8.ToArray()); w.Write(dataBytes);
        var bytes = new byte[dataBytes];
        Buffer.BlockCopy(pcm, 0, bytes, 0, dataBytes);
        w.Write(bytes);
    }
}
