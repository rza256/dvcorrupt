using System.Diagnostics;
using System.Text;

namespace DvCorrupt.Media;

public sealed class FfmpegProcess : IDisposable
{
    readonly Process proc;
    readonly StringBuilder err = new();
    public readonly Stream? Out;
    public readonly Stream? In;

    public FfmpegProcess(string args, bool readStdout, bool writeStdin)
    {
        var psi = new ProcessStartInfo(Tools.FFmpeg, args)
        {
            UseShellExecute = false,
            RedirectStandardOutput = readStdout,
            RedirectStandardInput = writeStdin,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        proc = Process.Start(psi) ?? throw new Exception("Could not start ffmpeg");
        proc.ErrorDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            lock (err) { if (err.Length < 16000) err.AppendLine(e.Data); }
        };
        proc.BeginErrorReadLine();
        if (readStdout) Out = proc.StandardOutput.BaseStream;
        if (writeStdin) In = proc.StandardInput.BaseStream;
    }

    public string ErrorText { get { lock (err) return err.ToString().Trim(); } }

    public bool ReadExact(byte[] buf, int count)
    {
        int off = 0;
        while (off < count)
        {
            int n = Out!.Read(buf, off, count - off);
            if (n <= 0) return false;
            off += n;
        }
        return true;
    }

    public byte[] ReadAll()
    {
        using var ms = new MemoryStream();
        Out!.CopyTo(ms);
        return ms.ToArray();
    }

    public int Finish()
    {
        try { In?.Close(); } catch { }
        proc.WaitForExit();
        return proc.ExitCode;
    }

    public void Dispose()
    {
        try { if (!proc.HasExited) proc.Kill(); } catch { }
        proc.Dispose();
    }
}
