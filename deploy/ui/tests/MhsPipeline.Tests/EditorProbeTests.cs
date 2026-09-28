// EditorProbe 测试：按"命令行含工程名"判定 —— 多工程并行时不能串台。

using System.Collections.Generic;
using MhsPipeline.Core;
using Xunit;

namespace MhsPipeline.Tests;

public class EditorProbeTests
{
    [Fact]
    public void Probe_True_WhenCommandLineHasProject()
    {
        var q = new FakeProcessQuery();
        q.Editors.Add(new EditorProcess(1,
            "D:/UE/UnrealEditor.exe F:/p/MH_Line01/MH_Line01.uproject -unattended -nosplash"));
        Assert.True(new EditorProbe(q).IsRunning("F:/p/MH_Line01/MH_Line01.uproject"));
    }

    [Fact]
    public void Probe_False_ForOtherProject_NoCrossTalk()
    {
        var q = new FakeProcessQuery();
        q.Editors.Add(new EditorProcess(1, "D:/UE/UnrealEditor.exe F:/p/MH_T01/MH_T01.uproject -unattended"));
        Assert.False(new EditorProbe(q).IsRunning("F:/p/MH_Line01/MH_Line01.uproject"));
    }

    [Fact]
    public void Probe_False_WhenNoEditorProcess()
    {
        Assert.False(new EditorProbe(new FakeProcessQuery()).IsRunning("F:/p/P.uproject"));
    }

    [Fact]
    public void Probe_False_WhenProjectEmpty()
    {
        var q = new FakeProcessQuery();
        q.Editors.Add(new EditorProcess(1, "UnrealEditor.exe"));
        Assert.False(new EditorProbe(q).IsRunning(""));
    }
}
