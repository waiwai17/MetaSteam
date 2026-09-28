// 测试用假文件系统：内容在内存，mtime 可精确指定 —— 不碰真实磁盘、可重复。

using System.Collections.Generic;
using MhsPipeline.Core;

namespace MhsPipeline.Tests;

public sealed class FakeFileSystem : IFileSystem
{
    private readonly Dictionary<string, string> _text = new();
    private readonly Dictionary<string, DateTime> _mtime = new();

    public void SetFile(string path, string content, DateTime? mtimeUtc = null)
    {
        _text[path] = content;
        _mtime[path] = mtimeUtc ?? DateTime.UtcNow;
    }

    public bool Exists(string path) => _text.ContainsKey(path);
    public string? ReadText(string path) => _text.TryGetValue(path, out var v) ? v : null;

    public DateTime? LastWriteUtc(string path) => _mtime.TryGetValue(path, out var v) ? v : null;

    /// <summary>模拟读取异常（权限/占用）——验证调用方不崩。</summary>
    public bool ThrowOnRead { get; set; }
}

public sealed class FakeRunner : IProcessRunner
{
    public List<(string File, string Args, bool Wait)> Calls { get; } = new();
    public int ExitCode { get; set; }
    public string Output { get; set; } = "";

    public CommandResult Run(string file, string args, bool wait)
    {
        Calls.Add((file, args, wait));
        return new CommandResult(ExitCode, Output);
    }
}

public sealed class FakeProcessQuery : IProcessQuery
{
    public List<EditorProcess> Editors { get; } = new();
    public IEnumerable<EditorProcess> GetEditors() => Editors;
}
