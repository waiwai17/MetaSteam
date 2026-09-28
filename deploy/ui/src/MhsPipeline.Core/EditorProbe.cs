// 引擎进程探测：必须与 launcher.find_running_editor 语义一致 ——
// 按"命令行里包含工程名"判定，而不是"只要有 UnrealEditor 进程就算"。
// 否则多工程并行时会把别人的引擎当成自己工程的（判定串台）。

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace MhsPipeline.Core;

public sealed record EditorProcess(int Pid, string CommandLine);

public interface IProcessQuery
{
    /// <summary>所有 UnrealEditor 进程（PID + 命令行）。</summary>
    IEnumerable<EditorProcess> GetEditors();
}

/// <summary>WMI 查询（可读命令行）；不可用时降级为进程名（无法匹配工程，调用方兜底）。</summary>
public sealed class WmiProcessQuery : IProcessQuery
{
    public IEnumerable<EditorProcess> GetEditors() => Query();

    private static IEnumerable<EditorProcess> Query()
    {
        var list = new List<EditorProcess>();
        try
        {
            var searcher = new System.Management.ManagementObjectSearcher(
                "SELECT ProcessId, CommandLine FROM Win32_Process WHERE Name='UnrealEditor.exe'");
            foreach (var o in searcher.Get())
            {
                var line = o["CommandLine"]?.ToString() ?? "";
                list.Add(new EditorProcess(Convert.ToInt32(o["ProcessId"]), line));
            }
        }
        catch
        {
            foreach (var p in System.Diagnostics.Process.GetProcessesByName("UnrealEditor"))
            {
                list.Add(new EditorProcess(p.Id, "UnrealEditor.exe"));
                p.Dispose();
            }
        }
        return list;
    }
}

public sealed class EditorProbe
{
    private readonly IProcessQuery _query;
    public EditorProbe(IProcessQuery query) { _query = query; }

    /// <summary>本工程是否有编辑器在跑（按工程文件名匹配命令行）。</summary>
    public bool IsRunning(string projectPath)
    {
        var key = Path.GetFileName(projectPath ?? "");
        if (string.IsNullOrEmpty(key)) return false;
        return _query.GetEditors()
            .Any(e => e.CommandLine.Contains(key, StringComparison.OrdinalIgnoreCase));
    }
}
