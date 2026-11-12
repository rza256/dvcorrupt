namespace DvCorrupt.Media;

/// <summary>Source file + how it is cropped/scaled into the 4:3 working raster.</summary>
public sealed class SourceSpec
{
    public readonly MediaInfo Info;
    public readonly int OutW, OutH;
    public readonly double CropOffset;   // -1 = left edge, 0 = centre, 1 = right edge
    public readonly bool Deinterlace;

    public SourceSpec(MediaInfo info, int outW, int outH, double cropOffset, bool deinterlace)
    {
        Info = info; OutW = outW; OutH = outH; CropOffset = cropOffset; Deinterlace = deinterlace;
    }

    /// <summary>Wider than 4:3 (e.g. HVX200 DVCPRO HD, 1280x1080 with 3:2 pixels = 16:9).</summary>
    public bool IsWide => Info.Dar > 4.0 / 3.0 + 0.02;
    public bool Interlaced => Info.EffectiveFieldOrder != "" && !Deinterlace;
    public double Fps => Info.Fps;
    public long FrameCount => Info.FrameCount;
    public int FrameBytes => OutW * OutH * 3;
    public bool IsSdNtscRaster => OutW == 720 && OutH == 480;
    public bool IsSdPalRaster => OutW == 720 && OutH == 576;

    /// <summary>
    /// Output frames per DV tape frame. Deinterlaced double-rate footage (e.g. HVX200 1080i
    /// bwdif'd to 59.94p) has one frame per field, so two frames share one tape frame.
    /// </summary>
    public int TapeStep => Info.EffectiveFieldOrder == "" && Info.Fps > (Info.IsPal ? 26 : 31)
        ? Math.Max(1, (int)Math.Round(Info.Fps / (Info.IsPal ? 25 : 30000.0 / 1001)))
        : 1;
    public bool IsDoubleRate => TapeStep == 2;

    public static List<(string Label, int W, int H)> OutputSizes(MediaInfo info)
    {
        var list = new List<(string, int, int)>();
        if (info.Dar > 4.0 / 3.0 + 0.02)
        {
            if (info.IsPal) list.Add(("720×576  DV PAL, 4:3", 720, 576));
            else list.Add(("720×480  DV NTSC, 4:3", 720, 480));
            list.Add(("960×720  4:3 HD", 960, 720));
            list.Add(("1440×1080  4:3 HD", 1440, 1080));
        }
        else
        {
            list.Add(($"{info.Width}×{info.Height}  native", info.Width, info.Height));
        }
        return list;
    }

    static int Even(double v) => Math.Max(2, (int)Math.Round(v / 2) * 2);

    public string BuildFilter()
    {
        var f = new List<string>();
        if (IsWide)
        {
            // stored-pixel width that displays as 4:3 at the source's pixel aspect ratio
            int cw = Math.Min(Info.Width, Even(Info.Height * 4.0 / 3.0 / Info.Sar));
            int maxX = Info.Width - cw;
            int cx = Math.Clamp(Even(maxX / 2.0 * (1 + CropOffset)), 0, maxX);
            f.Add($"crop={cw}:{Info.Height}:{cx}:0");
        }
        bool srcInterlaced = Info.EffectiveFieldOrder != "";
        if (Deinterlace && srcInterlaced) f.Add("bwdif=mode=send_frame:parity=auto:deint=all");
        string matrix = Info.Height > 576 ? "bt709" : "bt601";
        int interl = (srcInterlaced && !Deinterlace && OutH != Info.Height) ? 1 : 0;
        f.Add($"scale={OutW}:{OutH}:flags=lanczos:interl={interl}:in_color_matrix={matrix}:in_range=tv");
        f.Add("format=bgr24");
        return string.Join(",", f);
    }

    public string ReaderArgs(long startFrame)
    {
        double t = startFrame * Info.FpsDen / (double)Info.FpsNum;
        string ss = startFrame > 0 ? $"-ss {Tools.Num(t)} " : "";
        return $"-hide_banner -loglevel error -nostdin {ss}-i \"{Info.Path}\" -map 0:v:0 -an -sn -dn " +
               $"-vf \"{BuildFilter()}\" -f rawvideo -pix_fmt bgr24 -";
    }
}
