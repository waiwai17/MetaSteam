// 服务层：文件读取（可注入 → 完全可单测）、存活判定（语义与 launcher 保持一致）、
// 重绘去重、launcher CLI 调用封装。
//
// 关键一致性：DeriveState 的判定必须与 launcher.evaluate 相同 ——
// 空闲 + 心跳陈腐 = idle（正常，不误报故障），有活却没动静才是 stalled。

using System.Text.Json;

namespace MhsPipeline.Core;

public interface IFileSystem
{
    bool Exists(string path);
    string? ReadText(string path);
    DateTime? LastWriteUtc(string path);
}

public sealed class RealFileSystem : IFileSystem
{
    public bool Exists(string path) => File.Exists(path);
    public string? ReadText(string path)
    {
        try { return File.ReadAllText(path); } catch { return null; }
    }
    public DateTime? LastWriteUtc(string path)
    {
        try { return File.GetLastWriteTimeUtc(path); } catch { return null; }
    }
}

public static class Jsonx
{
    private static readonly JsonSerializerOptions Opts = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true
    };

    /// <summary>解析失败返回 null（调用方按"降级：跳过本轮"处理，绝不抛给 UI）。</summary>
    public static T? TryParse<T>(string? text) where T : class
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        try { return JsonSerializer.Deserialize<T>(text, Opts); }
        catch { return null; }
    }
}

public sealed class Paths
{
    public Paths(string projectPath)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(projectPath)) ?? "";
        Saved = Path.Combine(dir, "Saved");
        ConfigDir = Path.Combine(Saved, "Config", "MetaHumanSolver");
        State = Path.Combine(ConfigDir, "stream_state.json");
        Queue = Path.Combine(ConfigDir, "queue_state.json");
        Progress = Path.Combine(ConfigDir, "progress.json");
        EngineStatus = Path.Combine(ConfigDir, "pipeline.status.json");
        IdentityIndex = Path.Combine(ConfigDir, "identity_assets.json");
        StopFlag = Path.Combine(ConfigDir, "stop.flag");
        StopIntent = Path.Combine(ConfigDir, "stop.intent");
    }

    public string Saved { get; }
    public string ConfigDir { get; }
    public string State { get; }
    public string Queue { get; }
    public string Progress { get; }
    public string EngineStatus { get; }
    public string IdentityIndex { get; }
    public string StopFlag { get; }
    public string StopIntent { get; }
}

public sealed class StateReader
{
    public const double StallSeconds = 600.0;     // 与 launcher.STALL_SECONDS 一致
    private static readonly string[] ActiveStatuses = { "pending", "importing", "solving", "exporting" };

    private readonly IFileSystem _fs;
    public StateReader(IFileSystem fs) { _fs = fs; }

    public PipelineSnapshot? ReadState(Paths p) => Jsonx.TryParse<PipelineSnapshot>(_fs.ReadText(p.State));
    public QueueSnapshot? ReadQueue(Paths p) => Jsonx.TryParse<QueueSnapshot>(_fs.ReadText(p.Queue));
    public ProgressSnapshot? ReadProgress(Paths p) => Jsonx.TryParse<ProgressSnapshot>(_fs.ReadText(p.Progress));
    public EngineStatus? ReadEngineStatus(Paths p) => Jsonx.TryParse<EngineStatus>(_fs.ReadText(p.EngineStatus));
    public IdentityIndex? ReadIdentityIndex(Paths p) => Jsonx.TryParse<IdentityIndex>(_fs.ReadText(p.IdentityIndex));

    public int ActiveTaskCount(QueueSnapshot? q)
        => q?.Tasks.Count(t => ActiveStatuses.Contains(t.Status)) ?? 0;

    /// <summary>心跳年龄（state/progress 中较新者）；都读不到返回 null。</summary>
    public double? HeartbeatAgeSeconds(Paths p, DateTime nowUtc)
    {
        var a = Age(_fs.LastWriteUtc(p.State), nowUtc);
        var b = Age(_fs.LastWriteUtc(p.Progress), nowUtc);
        if (a is null && b is null) return null;
        if (a is null) return b;
        if (b is null) return a;
        return Math.Min(a.Value, b.Value);
    }

    private static double? Age(DateTime? ts, DateTime nowUtc)
        => ts is null ? null : (nowUtc - ts.Value).TotalSeconds;

    /// <summary>
    /// 派生状态（与 launcher.evaluate + stop.intent 语义一致）：
    /// healthy / idle / stalled / dead / stopped / paused。
    /// </summary>
    public string DeriveState(Paths p, bool engineAlive, DateTime nowUtc)
    {
        var active = ActiveTaskCount(ReadQueue(p));
        var age = HeartbeatAgeSeconds(p, nowUtc);

        if (!engineAlive)
        {
            // 人为暂停（有 intent）不算故障
            if (_fs.Exists(p.StopIntent)) return "paused";
            return active > 0 ? "dead" : "stopped";
        }
        if (age is not null && age <= StallSeconds) return "healthy";
        if (active > 0) return "stalled";
        if (_fs.Exists(p.StopFlag)) return "paused";   // 已请求暂停，等当前段完成
        return "idle";
    }
}

/// <summary>内容无变化则不重绘（沿用控制台"帧去重"思路，避免 UI 自耗）。</summary>
public sealed class RenderDeduplicator
{
    private string _last = "";
    public bool ShouldRender(string content)
    {
        if (content == _last) return false;
        _last = content;
        return true;
    }
}

public sealed record CommandResult(int ExitCode, string Output);

public interface IProcessRunner
{
    CommandResult Run(string file, string args, bool wait);
}

public sealed class RealProcessRunner : IProcessRunner
{
    public CommandResult Run(string file, string args, bool wait)
    {
        var psi = new System.Diagnostics.ProcessStartInfo(file, args)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            // 分离命令（wait=false）绝不重定向 stdout：父进程从不读管道，
            // Process.Dispose 关闭句柄后子进程一打印就死 ——
            // 曾导致 GUI [启动] 只写出 job_stream.json，watchdog/status 永远不出现
            RedirectStandardOutput = wait
        };
        using var proc = System.Diagnostics.Process.Start(psi);
        if (proc is null) return new CommandResult(-1, "");
        if (!wait) return new CommandResult(0, "");            // 启动类命令：分离执行
        var outp = proc.StandardOutput.ReadToEnd();
        proc.WaitForExit();
        return new CommandResult(proc.ExitCode, outp);
    }
}

public sealed class LauncherClient
{
    private readonly IProcessRunner _runner;
    public string Python { get; set; } = "python";
    public string LauncherPath { get; set; } = "launcher.py";
    /// <summary>分离命令的输出日志（追加写）。为空则用 launcher 同名 .gui.log。</summary>
    public string LogPath { get; set; } = "";

    public LauncherClient(IProcessRunner runner, string? launcherPath = null)
    {
        _runner = runner;
        if (launcherPath is not null) LauncherPath = launcherPath;
    }

    private CommandResult Exec(string args, bool wait)
    {
        if (wait) return _runner.Run(Python, $"\"{LauncherPath}\" {args}", true);
        // 分离命令经 cmd 转发并把输出追加到日志文件：GUI 不持管道（防子进程死），
        // 且失败可事后诊断（无日志 = 失败只能靠猜，曾让 [启动] 看起来"毫无反馈"）
        var log = string.IsNullOrEmpty(LogPath)
            ? System.IO.Path.ChangeExtension(LauncherPath, ".gui.log") : LogPath;
        return _runner.Run("cmd.exe",
            $"/c \"\"{Python}\" \"{LauncherPath}\" {args} >> \"{log}\" 2>&1\"", false);
    }

    public CommandResult Start(string config, bool showWindow = false)
        => Exec($"start --config \"{config}\"" + (showWindow ? " --show-window" : ""), wait: false);

    public CommandResult Pause(string config) => Exec($"pause --config \"{config}\"", wait: true);
    public CommandResult Status(string config) => Exec($"status --config \"{config}\"", wait: true);
    public CommandResult Doctor(string config) => Exec($"doctor --config \"{config}\"", wait: true);

    public CommandResult NewProject(string name, string dir)
        => Exec($"new --name \"{name}\" --dir \"{dir}\"", wait: true);
}
