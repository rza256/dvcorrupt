using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using DvCorrupt.Core;
using DvCorrupt.Media;
using Microsoft.Win32;

namespace DvCorrupt;

public partial class MainWindow : Window
{
    readonly Player player = new();
    GlitchParams prm = new();
    MediaInfo? info;
    SourceSpec? spec;
    WriteableBitmap? frameBmp, scopeBmp;
    int uiPending;
    long curFrame;
    long? inFrame, outFrame;
    bool suppress;
    bool playing;
    CancellationTokenSource? exportCts;
    readonly List<Action> refreshers = new();
    readonly DispatcherTimer cropDebounce = new() { Interval = TimeSpan.FromMilliseconds(180) };
    List<(string Label, int W, int H)> sizes = new();

    public MainWindow()
    {
        InitializeComponent();
        BuildParamUi();

        suppress = true;
        PresetBox.Items.Add("(custom)");
        foreach (var p in GlitchParams.Presets) PresetBox.Items.Add(p.Name);
        PresetBox.SelectedIndex = 1;
        foreach (var c in ExportJob.Codecs) CodecBox.Items.Add(c.Label);
        CodecBox.SelectedIndex = 0;
        suppress = false;

        player.Presented += OnPresented;
        player.PlayStateChanged += pl => Dispatcher.BeginInvoke(() =>
        {
            playing = pl;
            PlayBtn.Content = pl ? "❚❚  Pause" : "▶  Play";
        });
        player.Error += msg => Dispatcher.BeginInvoke(() => StatusText.Text = "Error: " + msg);
        player.SetParams(prm.Clone());

        cropDebounce.Tick += (_, _) => { cropDebounce.Stop(); ApplySourceOptions(); };
        CropSlider.MouseDoubleClick += (_, _) => CropSlider.Value = 0;

        PreviewKeyDown += OnKey;
        Drop += (_, e) =>
        {
            if (e.Data.GetData(DataFormats.FileDrop) is string[] files && files.Length > 0) OpenFile(files[0]);
        };
        Closed += (_, _) => { exportCts?.Cancel(); player.Dispose(); };
        UpdateSourceUi();
    }

    // ------------------------------------------------------------------ parameters UI

    void BuildParamUi()
    {
        int insertAt = ParamPanel.Children.IndexOf(ExportExpander);
        foreach (var group in GlitchParams.Defs.GroupBy(d => d.Group))
        {
            var stack = new StackPanel { Margin = new Thickness(4, 2, 0, 0) };
            var exp = new Expander { Header = group.Key, IsExpanded = true, Content = stack };
            foreach (var d in group) stack.Children.Add(BuildControl(d));
            ParamPanel.Children.Insert(insertAt++, exp);
        }
    }

    FrameworkElement BuildControl(ParamDef d)
    {
        var dim = (Brush)FindResource("Dim");
        switch (d.Kind)
        {
            case ParamKind.Bool:
            {
                var cb = new CheckBox { Content = d.Label, Margin = new Thickness(0, 6, 0, 2), ToolTip = Tip(d) };
                cb.Click += (_, _) => { d.Set(prm, cb.IsChecked == true ? 1 : 0); ParamsChanged(); };
                refreshers.Add(() => cb.IsChecked = d.Get(prm) > 0.5);
                cb.IsChecked = d.Get(prm) > 0.5;
                return cb;
            }
            case ParamKind.Enum:
            {
                var panel = new StackPanel { Margin = new Thickness(0, 4, 0, 4) };
                panel.Children.Add(new TextBlock { Text = d.Label, Foreground = dim, Margin = new Thickness(0, 0, 0, 3), ToolTip = Tip(d) });
                var combo = new ComboBox { ToolTip = Tip(d) };
                foreach (var o in d.Options!) combo.Items.Add(o);
                combo.SelectedIndex = (int)d.Get(prm);
                combo.SelectionChanged += (_, _) =>
                {
                    if (suppress || combo.SelectedIndex < 0) return;
                    d.Set(prm, combo.SelectedIndex);
                    ParamsChanged();
                };
                refreshers.Add(() => combo.SelectedIndex = (int)d.Get(prm));
                panel.Children.Add(combo);
                return panel;
            }
            default:
            {
                var panel = new StackPanel { Margin = new Thickness(0, 3, 0, 1), ToolTip = Tip(d) };
                var head = new DockPanel();
                var label = new TextBlock { Text = d.Label, Foreground = dim };
                var val = new TextBlock { FontFamily = new FontFamily("Consolas"), HorizontalAlignment = HorizontalAlignment.Right };
                DockPanel.SetDock(val, Dock.Right);
                head.Children.Add(val);
                head.Children.Add(label);
                var slider = new Slider
                {
                    Minimum = d.Min, Maximum = d.Max, Value = d.Get(prm),
                    IsMoveToPointEnabled = true, Focusable = false,
                    SmallChange = (d.Max - d.Min) / 200, LargeChange = (d.Max - d.Min) / 20,
                };
                if (d.Kind == ParamKind.Int) { slider.IsSnapToTickEnabled = true; slider.TickFrequency = 1; }
                double def = d.Get(new GlitchParams());
                void ShowVal() => val.Text = d.Get(prm).ToString(d.Fmt);
                slider.ValueChanged += (_, e) =>
                {
                    if (suppress) return;
                    d.Set(prm, e.NewValue);
                    ShowVal();
                    ParamsChanged();
                };
                // double-click the label row to reset to default
                head.MouseLeftButtonDown += (_, e) => { if (e.ClickCount == 2) slider.Value = def; };
                refreshers.Add(() => { slider.Value = d.Get(prm); ShowVal(); });
                ShowVal();
                panel.Children.Add(head);
                panel.Children.Add(slider);
                return panel;
            }
        }
    }

    static string? Tip(ParamDef d) => string.IsNullOrEmpty(d.Tip) ? null : d.Tip + "\n(double-click the name to reset)";

    void ParamsChanged(bool markCustom = true)
    {
        if (markCustom && !suppress && PresetBox.SelectedIndex != 0)
        {
            suppress = true;
            PresetBox.SelectedIndex = 0;
            suppress = false;
        }
        player.SetParams(prm.Clone());
    }

    void RefreshAll()
    {
        suppress = true;
        foreach (var r in refreshers) r();
        suppress = false;
    }

    void Preset_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (suppress || PresetBox.SelectedIndex <= 0) return;
        var (_, apply) = GlitchParams.Presets[PresetBox.SelectedIndex - 1];
        var p = new GlitchParams { Seed = prm.Seed, ShowMap = prm.ShowMap };
        apply(p);
        prm = p;
        RefreshAll();
        ParamsChanged(markCustom: false);
    }

    void SavePreset_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new SaveFileDialog { Filter = "DV Corrupt preset|*.dvcpreset.json|JSON|*.json", FileName = "preset.dvcpreset.json" };
        if (dlg.ShowDialog(this) == true) File.WriteAllText(dlg.FileName, prm.ToJson());
    }

    void LoadPreset_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "DV Corrupt preset|*.dvcpreset.json;*.json|All files|*.*" };
        if (dlg.ShowDialog(this) != true) return;
        try
        {
            prm = GlitchParams.FromJson(File.ReadAllText(dlg.FileName));
            RefreshAll();
            suppress = true; PresetBox.SelectedIndex = 0; suppress = false;
            ParamsChanged(markCustom: false);
        }
        catch (Exception ex) { StatusText.Text = "Could not load preset: " + ex.Message; }
    }

    // ------------------------------------------------------------------ source

    void Open_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "Video (DV, P2, MP4)|*.avi;*.mxf;*.dv;*.mov;*.mp4;*.m4v;*.mkv|All files|*.*" };
        if (dlg.ShowDialog(this) == true) OpenFile(dlg.FileName);
    }

    public async void OpenFile(string path)
    {
        StatusText.Text = "Probing " + System.IO.Path.GetFileName(path) + "…";
        try
        {
            var mi = await Task.Run(() => MediaInfo.Probe(path));
            info = mi;
            inFrame = outFrame = null;
            sizes = SourceSpec.OutputSizes(mi);
            suppress = true;
            SizeBox.Items.Clear();
            foreach (var s in sizes) SizeBox.Items.Add(s.Label);
            SizeBox.SelectedIndex = 0;
            CropSlider.Value = 0;
            DeintBox.IsChecked = false;
            suppress = false;
            FileText.Text = $"{System.IO.Path.GetFileName(path)}  ·  {mi.Codec} {mi.Width}×{mi.Height} {mi.PixFmt}  ·  {mi.Fps:0.###} fps  ·  " +
                            $"{TimeSpan.FromSeconds(mi.Duration):hh\\:mm\\:ss}" + (mi.HasAudio ? "  ·  audio" : "  ·  no audio");
            Placeholder.Visibility = Visibility.Collapsed;
            SeekSlider.Maximum = Math.Max(1, mi.FrameCount - 1);
            UpdateSourceUi();
            UpdateRangeUi();
            curFrame = 0;
            ApplySourceOptions();
            StatusText.Text = SourceSpecDescription();
        }
        catch (Exception ex)
        {
            StatusText.Text = "Could not open: " + ex.Message;
        }
    }

    string SourceSpecDescription()
    {
        if (spec == null) return "";
        string s = spec.IsWide
            ? $"16:9 source → 4:3 crop → {spec.OutW}×{spec.OutH}"
            : $"{spec.OutW}×{spec.OutH} 4:3";
        string scan = spec.Interlaced ? $", interlaced ({spec.Info.EffectiveFieldOrder})"
            : spec.IsDoubleRate ? $", {spec.Fps:0.##}p double-rate (frame pairs share one tape frame)"
            : ", progressive";
        return s + scan + $", DV {(spec.Info.IsPal ? "625/50 (12 tracks/frame)" : "525/60 (10 tracks/frame)")} error model";
    }

    void UpdateSourceUi()
    {
        bool wide = info != null && info.Dar > 4.0 / 3.0 + 0.02;
        SizeBox.IsEnabled = wide;
        CropSlider.IsEnabled = wide;
        DeintBox.IsEnabled = info != null && info.EffectiveFieldOrder != "";
        bool doubleRate = info != null && new SourceSpec(info, 720, 480, 0, false).IsDoubleRate;
        ReinterlaceBox.Visibility = doubleRate ? Visibility.Visible : Visibility.Collapsed;
        if (info != null)
            ReinterlaceBox.Content = info.IsPal ? "Re-interlace 50p → 25i (field pairs)" : "Re-interlace 59.94p → 29.97i (field pairs)";
    }

    void SourceOption_Changed(object sender, RoutedEventArgs e)
    {
        if (!suppress) ApplySourceOptions();
    }

    void Crop_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        double v = CropSlider.Value;
        CropText.Text = Math.Abs(v) < 0.01 ? "centre" : v < 0 ? $"left {-v:0.00}" : $"right {v:0.00}";
        if (suppress) return;
        cropDebounce.Stop();
        cropDebounce.Start();
    }

    void ApplySourceOptions()
    {
        if (info == null) return;
        int idx = Math.Max(0, SizeBox.SelectedIndex);
        var sz = sizes[Math.Min(idx, sizes.Count - 1)];
        spec = new SourceSpec(info, sz.W, sz.H, CropSlider.Value, DeintBox.IsChecked == true);
        player.Open(spec, curFrame);
        StatusText.Text = SourceSpecDescription();
    }

    // ------------------------------------------------------------------ presentation

    void OnPresented()
    {
        if (Interlocked.Exchange(ref uiPending, 1) == 1) return;
        Dispatcher.BeginInvoke(DispatcherPriority.Render, () =>
        {
            Interlocked.Exchange(ref uiPending, 0);
            lock (player.PresentLock)
            {
                int w = player.PresentW, h = player.PresentH;
                if (w == 0) return;
                if (frameBmp == null || frameBmp.PixelWidth != w || frameBmp.PixelHeight != h)
                {
                    frameBmp = new WriteableBitmap(w, h, 96, 96, PixelFormats.Bgr24, null);
                    Preview.Source = frameBmp;
                }
                frameBmp.WritePixels(new Int32Rect(0, 0, w, h), player.PresentFrame, w * 3, 0);
                if (scopeBmp == null)
                {
                    scopeBmp = new WriteableBitmap(ScopeRenderer.W, ScopeRenderer.H, 96, 96, PixelFormats.Bgr24, null);
                    ScopeImg.Source = scopeBmp;
                }
                scopeBmp.WritePixels(new Int32Rect(0, 0, ScopeRenderer.W, ScopeRenderer.H), player.PresentScope, ScopeRenderer.W * 3, 0);
                curFrame = player.PresentIndex;
                int total = (spec?.Info.IsPal ?? false) ? 12 * 135 : 10 * 135;
                StatsText.Text = $"concealed {player.PresentVideoErrors,4}/{total} MB  audio blk {player.PresentAudioErrors,3}  " +
                                 $"head A {player.PresentGainA * 100,3:0}%  B {player.PresentGainB * 100,3:0}%";
            }
            suppress = true;
            SeekSlider.Value = curFrame;
            suppress = false;
            TimeText.Text = Timecode(curFrame);
        });
    }

    string Timecode(long f)
    {
        if (info == null) return "00:00:00;00";
        int nominal = (int)Math.Round(info.Fps);
        long secs = f / nominal;
        return $"{secs / 3600:00}:{secs / 60 % 60:00}:{secs % 60:00};{f % nominal:00}";
    }

    // ------------------------------------------------------------------ transport

    void Play_Click(object sender, RoutedEventArgs e) => TogglePlay();
    void TogglePlay() { if (spec != null) player.SetPlaying(!playing); }
    void Back_Click(object sender, RoutedEventArgs e) => Step(-1);
    void Fwd_Click(object sender, RoutedEventArgs e) => Step(1);

    void Step(int d)
    {
        if (spec == null) return;
        if (playing) player.SetPlaying(false);
        long t = Math.Clamp(curFrame + d, 0, Math.Max(0, spec.FrameCount - 1));
        curFrame = t;
        player.Seek(t);
    }

    void Seek_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (suppress || spec == null) return;
        long f = (long)Math.Round(e.NewValue);
        curFrame = f;
        player.Seek(f);
        TimeText.Text = Timecode(f);
    }

    void OnKey(object sender, KeyEventArgs e)
    {
        if (e.OriginalSource is TextBox) return;
        switch (e.Key)
        {
            case Key.Space: TogglePlay(); e.Handled = true; break;
            case Key.Left: Step(Keyboard.Modifiers == ModifierKeys.Shift ? -10 : -1); e.Handled = true; break;
            case Key.Right: Step(Keyboard.Modifiers == ModifierKeys.Shift ? 10 : 1); e.Handled = true; break;
            case Key.I: SetIn_Click(this, e); e.Handled = true; break;
            case Key.O: SetOut_Click(this, e); e.Handled = true; break;
        }
    }

    void SetIn_Click(object sender, RoutedEventArgs e)
    {
        if (info == null) return;
        inFrame = curFrame;
        if (outFrame <= inFrame) outFrame = null;
        UpdateRangeUi();
    }

    void SetOut_Click(object sender, RoutedEventArgs e)
    {
        if (info == null) return;
        outFrame = curFrame + 1;
        if (inFrame >= outFrame) inFrame = null;
        UpdateRangeUi();
    }

    void ClearInOut_Click(object sender, RoutedEventArgs e)
    {
        inFrame = outFrame = null;
        UpdateRangeUi();
    }

    (long In, long Out) Range() => (inFrame ?? 0, outFrame ?? (info?.FrameCount ?? 0));

    void UpdateRangeUi()
    {
        var (a, b) = Range();
        SeekSlider.SelectionStart = a;
        SeekSlider.SelectionEnd = Math.Max(a, b - 1);
        RangeText.Text = inFrame == null && outFrame == null
            ? "Range: whole file"
            : $"Range: {Timecode(a)} → {Timecode(b)}  ({b - a} frames)";
    }

    // ------------------------------------------------------------------ export

    async void Export_Click(object sender, RoutedEventArgs e)
    {
        if (spec == null || info == null) { StatusText.Text = "Open a video first."; return; }
        var codec = ExportJob.Codecs[Math.Max(0, CodecBox.SelectedIndex)];
        var dlg = new SaveFileDialog
        {
            Filter = $"{codec.Ext.ToUpperInvariant()}|*.{codec.Ext}",
            FileName = System.IO.Path.GetFileNameWithoutExtension(info.Path) + "_corrupt." + codec.Ext,
            InitialDirectory = System.IO.Path.GetDirectoryName(info.Path),
        };
        if (dlg.ShowDialog(this) != true) return;
        if (System.IO.Path.GetFullPath(dlg.FileName).Equals(System.IO.Path.GetFullPath(info.Path), StringComparison.OrdinalIgnoreCase))
        {
            StatusText.Text = "Refusing to overwrite the source file.";
            return;
        }

        var (a, b) = Range();
        var job = new ExportJob
        {
            OutputPath = dlg.FileName,
            Codec = codec.Codec,
            IncludeAudio = AudioBox.IsChecked == true,
            GlitchAudio = AudioGlitchBox.IsChecked == true,
            Reinterlace = ReinterlaceBox.IsChecked == true,
            InFrame = a,
            OutFrame = b,
        };
        var s = spec;
        var p = prm.Clone();
        exportCts = new CancellationTokenSource();
        var ct = exportCts.Token;
        ExportBtn.IsEnabled = false;
        CancelBtn.IsEnabled = true;
        ExportProgress.Value = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            await Task.Run(() => Exporter.Run(s, p, job, (v, msg) => Dispatcher.BeginInvoke(() =>
            {
                ExportProgress.Value = v;
                StatusText.Text = "Exporting: " + msg;
            }), ct));
            StatusText.Text = $"Exported {System.IO.Path.GetFileName(job.OutputPath)} in {sw.Elapsed:mm\\:ss}.";
            ExportProgress.Value = 1;
        }
        catch (OperationCanceledException)
        {
            StatusText.Text = "Export cancelled.";
            try { File.Delete(job.OutputPath); } catch { }
        }
        catch (Exception ex)
        {
            StatusText.Text = "Export failed: " + ex.Message;
        }
        finally
        {
            ExportBtn.IsEnabled = true;
            CancelBtn.IsEnabled = false;
        }
    }

    void Cancel_Click(object sender, RoutedEventArgs e) => exportCts?.Cancel();
}
