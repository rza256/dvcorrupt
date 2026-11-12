namespace DvCorrupt.Core;

public readonly struct RectI
{
    public readonly int X, Y, W, H;
    public RectI(int x, int y, int w, int h) { X = x; Y = y; W = w; H = h; }
}

/// <summary>
/// Where each compressed DV macroblock lives on screen.
///
/// A DV25 frame is recorded as 10 (525/60) or 12 (625/50) DIF sequences, one per helical
/// track. Each sequence holds 27 video segments of 5 macroblocks = 135 video DIF blocks.
/// The 5 macroblocks of a segment are deliberately scattered across 5 super-block columns
/// and 5 different super-block rows (the "shuffle"), so that a burst error on tape is spread
/// thinly over the picture. This port of the IEC 61834 shuffle (same tables as ffmpeg's
/// dv_calc_mb_coordinates) is what makes the concealment pattern look like a real deck.
///
/// For non-SD output sizes the layout is computed on the virtual SD raster and scaled.
/// </summary>
public sealed class DvLayout
{
    public const int Segments = 27;
    public const int VideoBlocks = 135;

    static readonly int[] Off = { 2, 6, 8, 0, 4 };
    static readonly int[] LStartShuffled = { 9, 4, 13, 0, 18 };
    static readonly int[] Shuf3 = { 18, 9, 27, 0, 36 };
    static readonly int[] Serpent1 =
    {
        0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2
    };
    static readonly int[] Serpent2 =
    {
        0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0,
        0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0,
        0, 1, 2, 3, 4, 5
    };

    public readonly bool Pal;
    public readonly int Tracks, Width, Height;
    /// <summary>Indexed [track * 135 + videoBlock]; videoBlock = segment * 5 + m.</summary>
    public readonly RectI[] Rects;
    /// <summary>Normalised diagonal position of each block (0 = top-left, 1 = bottom-right).</summary>
    public readonly float[] UDiag;
    /// <summary>Normalised raster-scan position of each block.</summary>
    public readonly float[] URaster;

    public DvLayout(bool pal, int width, int height)
    {
        Pal = pal;
        Tracks = pal ? 12 : 10;
        Width = width;
        Height = height;
        int rw = 720, rh = pal ? 576 : 480;
        double sx = width / (double)rw, sy = height / (double)rh;
        int mbRows = pal ? 36 : 60;

        Rects = new RectI[Tracks * VideoBlocks];
        UDiag = new float[Tracks * VideoBlocks];
        URaster = new float[Tracks * VideoBlocks];

        for (int s = 0; s < Tracks; s++)
        for (int j = 0; j < Segments; j++)
        for (int m = 0; m < 5; m++)
        {
            int x, y, w, h;
            if (pal)
            {
                // 4:2:0 625/50: 16x16 macroblocks, super block = 9x3 MBs
                x = (Shuf3[m] + j / 3) * 16;
                y = (Serpent1[j] + ((s + Off[m]) % 12) * 3) * 16;
                w = h = 16;
            }
            else
            {
                // 4:1:1 525/60: 32x8 macroblocks, right-most column uses 16x16 MBs
                int i = (s + Off[m]) % 10;
                int k = j + ((m == 1 || m == 2) ? 3 : 0);
                int cx = LStartShuffled[m] + k / 6;
                int cy = Serpent2[k] + i * 6;
                if (cx > 21)
                {
                    cy = cy * 2 - i * 6;
                    x = cx * 32; y = cy * 8; w = 16; h = 16;
                }
                else
                {
                    x = cx * 32; y = cy * 8; w = 32; h = 8;
                }
            }

            int idx = s * VideoBlocks + j * 5 + m;
            int x0 = (int)Math.Round(x * sx), x1 = (int)Math.Round((x + w) * sx);
            int y0 = (int)Math.Round(y * sy), y1 = (int)Math.Round((y + h) * sy);
            x0 = Math.Clamp(x0, 0, width); x1 = Math.Clamp(x1, 0, width);
            y0 = Math.Clamp(y0, 0, height); y1 = Math.Clamp(y1, 0, height);
            Rects[idx] = new RectI(x0, y0, x1 - x0, y1 - y0);

            double cxn = (x + w / 2.0) / rw, cyn = (y + h / 2.0) / rh;
            UDiag[idx] = (float)((cxn + cyn) / 2);
            URaster[idx] = (float)((Math.Floor(cyn * mbRows) + cxn) / mbRows);
        }
    }

    /// <summary>Returns number of pixels covered != 1 time (0 means the layout tiles the frame).</summary>
    public int CoverageErrors()
    {
        var cov = new byte[Width * Height];
        foreach (var r in Rects)
            for (int y = r.Y; y < r.Y + r.H; y++)
                for (int x = r.X; x < r.X + r.W; x++)
                    cov[y * Width + x]++;
        int bad = 0;
        foreach (var c in cov) if (c != 1) bad++;
        return bad;
    }
}
