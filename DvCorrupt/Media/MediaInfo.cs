using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

namespace DvCorrupt.Media;

public static class Tools
{
    static string? Find(string name)
    {
        string exe = name + ".exe";
        string local = System.IO.Path.Combine(AppContext.BaseDirectory, exe);
        if (File.Exists(local)) return local;
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
        {
            try
            {
                if (string.IsNullOrWhiteSpace(dir)) continue;
                string c = System.IO.Path.Combine(dir.Trim('"'), exe);
                if (File.Exists(c)) return c;
            }
            catch { }
        }
        return null;
    }

    public static string FFmpeg => Find("ffmpeg") ?? throw new FileNotFoundException("ffmpeg.exe not found on PATH or next to DvCorrupt.exe");
    public static string FFprobe => Find("ffprobe") ?? throw new FileNotFoundException("ffprobe.exe not found on PATH or next to DvCorrupt.exe");

    public static string Num(double v) => v.ToString("0.######", CultureInfo.InvariantCulture);
}

public sealed class MediaInfo
{
    public string Path = "";
    public int Width, Height;
    public int SarNum = 1, SarDen = 1;
    public int FpsNum = 30000, FpsDen = 1001;
    public double Duration;
    public long FrameCount;
    public bool HasAudio;
    public string FieldOrder = "";
    public string PixFmt = "";
    public string Codec = "";

    public double Fps => FpsNum / (double)FpsDen;
    public double Sar => SarNum / (double)SarDen;
    public double Dar => Width * Sar / Height;
    public bool IsPal => Math.Abs(Fps - 25) < 0.1 || Math.Abs(Fps - 50) < 0.1;

    /// <summary>"tt", "bb" or "" (progressive).</summary>
    public string EffectiveFieldOrder
    {
        get
        {
            if (FieldOrder is "tt" or "tb") return "tt";
            if (FieldOrder is "bb" or "bt") return "bb";
            if (FieldOrder == "progressive") return "";
            // DV25 SD is always interlaced, bottom field first
            if (Codec == "dvvideo" && Height <= 576) return "bb";
            if (Codec == "dvvideo") return "tt";
            return "";
        }
    }

    static (int, int) Ratio(string? s, int dn, int dd)
    {
        if (string.IsNullOrEmpty(s)) return (dn, dd);
        var parts = s.Split(':', '/');
        if (parts.Length == 2 && int.TryParse(parts[0], out int a) && int.TryParse(parts[1], out int b) && a > 0 && b > 0)
            return (a, b);
        return (dn, dd);
    }

    public static MediaInfo Probe(string path)
    {
        var psi = new ProcessStartInfo(Tools.FFprobe,
            $"-v error -print_format json -show_streams -show_format \"{path}\"")
        {
            UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true
        };
        using var p = Process.Start(psi)!;
        var errTask = p.StandardError.ReadToEndAsync();
        string json = p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        if (p.ExitCode != 0) throw new Exception("ffprobe failed: " + errTask.Result);

        using var doc = JsonDocument.Parse(json);
        var info = new MediaInfo { Path = path };
        bool gotVideo = false;
        foreach (var s in doc.RootElement.GetProperty("streams").EnumerateArray())
        {
            string type = s.TryGetProperty("codec_type", out var ct) ? ct.GetString() ?? "" : "";
            if (type == "video" && !gotVideo)
            {
                gotVideo = true;
                info.Width = s.GetProperty("width").GetInt32();
                info.Height = s.GetProperty("height").GetInt32();
                info.Codec = s.TryGetProperty("codec_name", out var cn) ? cn.GetString() ?? "" : "";
                info.PixFmt = s.TryGetProperty("pix_fmt", out var pf) ? pf.GetString() ?? "" : "";
                info.FieldOrder = s.TryGetProperty("field_order", out var fo) ? fo.GetString() ?? "" : "";
                (info.SarNum, info.SarDen) = Ratio(s.TryGetProperty("sample_aspect_ratio", out var sar) ? sar.GetString() : null, 1, 1);
                (info.FpsNum, info.FpsDen) = Ratio(s.TryGetProperty("r_frame_rate", out var fr) ? fr.GetString() : null, 30000, 1001);
                if (s.TryGetProperty("duration", out var d) && double.TryParse(d.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out double dur))
                    info.Duration = dur;
                if (s.TryGetProperty("nb_frames", out var nf) && long.TryParse(nf.GetString(), out long nfl))
                    info.FrameCount = nfl;
            }
            else if (type == "audio") info.HasAudio = true;
        }
        if (!gotVideo) throw new Exception("No video stream in file.");
        if (info.Duration <= 0 && doc.RootElement.TryGetProperty("format", out var fmt) &&
            fmt.TryGetProperty("duration", out var fd) &&
            double.TryParse(fd.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out double fdur))
            info.Duration = fdur;
        if (info.FrameCount <= 0) info.FrameCount = (long)Math.Floor(info.Duration * info.Fps + 0.5);
        return info;
    }
}
