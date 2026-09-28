// 存储与控制层测试：ConfigStore / BindingStore / GroupOptions / EngineControl(强制结束) /
// 身份选项 / 事件筛选 / VM 新构造。

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using MhsPipeline.Core;
using Xunit;

namespace MhsPipeline.Tests;

public sealed class RecordingQuery : IProcessQuery
{
    public List<EditorProcess> Editors { get; } = new();
    public IEnumerable<EditorProcess> GetEditors() => Editors;
}

public class StoresTests : IDisposable
{
    private readonly string _tmp = Path.Combine(Path.GetTempPath(),
        "mhs_ui_" + Guid.NewGuid().ToString("N"));

    private string Tmp(string rel) { Directory.CreateDirectory(_tmp); return Path.Combine(_tmp, rel); }

    public void Dispose()
    {
        try { Directory.Delete(_tmp, recursive: true); } catch { }
    }

    // ── ConfigStore ──
    [Fact]
    public void Config_RoundTrip_SaveThenLoadEquals()
    {
        var path = Tmp("pipeline.config.json");
        var cfg = new PipelineConfig
        {
            Instance = "line01", Project = "F:/p/P.uproject", Inbox = "E:/in",
            FbxOutput = "E:/out", IdentityImportRoot = "/Game/ID4",
            ImportRoot = "/Game/Auto6", WebPort = 8903
        };
        ConfigStore.Save(path, cfg);
        Assert.True(File.Exists(path));
        Assert.False(File.Exists(path + ".tmp"));                    // 原子写无残留
        var back = ConfigStore.Load(new RealFileSystem(), path);
        Assert.NotNull(back);
        Assert.Equal("line01", back!.Instance);
        Assert.Equal("/Game/ID4", back.IdentityImportRoot);
        Assert.Equal(8903, back.WebPort);
    }

    [Fact]
    public void Config_Load_MissingOrNull_ReturnsNull()
    {
        Assert.Null(ConfigStore.Load(new RealFileSystem(), Tmp("nope.json")));
        File.WriteAllText(Tmp("bad.json"), "{broken");
        Assert.Null(ConfigStore.Load(new RealFileSystem(), Tmp("bad.json")));
    }

    // ── BindingStore ──
    [Fact]
    public void Binding_Set_Then_Clear_ChangeReported()
    {
        var path = Tmp("bindings.json");
        var store = new BindingStore(new RealFileSystem(), path);

        Assert.True(store.Set("am", "/Game/ID/ID_am"));               // 新增 → 变化
        var all = store.Load();
        Assert.Equal("/Game/ID/ID_am", all["am"]);

        Assert.False(store.Set("am", "/Game/ID/ID_am"));              // 相同 → 无变化

        Assert.True(store.Set("am", "/Game/ID/ID_am2"));              // 覆盖 → 变化
        Assert.Equal("/Game/ID/ID_am2", store.Load()["am"]);

        Assert.True(store.Set("am", ""));                             // 清除 → 变化
        Assert.False(store.Load().ContainsKey("am"));
        Assert.False(store.Set("am", ""));                            // 再清除 → 无变化
    }

    [Fact]
    public void Binding_RootGroup_Works()
    {
        var path = Tmp("bindings.json");
        var store = new BindingStore(new RealFileSystem(), path);
        Assert.True(store.Set("", "/Game/ID/global"));
        Assert.Equal("/Game/ID/global", store.Load()[""]);
    }

    // ── GroupOptions ──
    [Fact]
    public void Groups_FromInbox_ExcludesId_RootLast()
    {
        var inbox = Tmp("inbox");
        foreach (var g in new[] { "pm", "am", "_id" })
            Directory.CreateDirectory(Path.Combine(inbox, g));

        var opts = GroupOptions.List(new RealFileSystem(), inbox);
        Assert.Equal(new[] { "am/", "pm/", "(根 · 全部)" }, opts.Select(o => o.Display).ToArray());
        Assert.Equal("", opts[^1].Value);                             // 根组值为空串
    }

    [Fact]
    public void Groups_MissingInbox_OnlyRoot()
    {
        var opts = GroupOptions.List(new RealFileSystem(), Tmp("no_such_inbox"));
        var single = Assert.Single(opts);
        Assert.Equal("(根 · 全部)", single.Display);
    }

    // ── EngineControl.ForceStop ──
    [Fact]
    public void ForceStop_WritesIntent_AndKillsOnlyMatching()
    {
        var project = "F:/p/MH_Line01/MH_Line01.uproject";
        var paths = new Paths(project);
        Directory.CreateDirectory(paths.ConfigDir);
        var query = new RecordingQuery();
        query.Editors.Add(new EditorProcess(111, $"D:/UE/UnrealEditor.exe {project} -unattended"));
        query.Editors.Add(new EditorProcess(222, "D:/UE/UnrealEditor.exe F:/other/X.uproject"));
        var killed = new List<int>();
        var ctl = new EngineControl(query, pid => { killed.Add(pid); return true; });

        var n = ctl.ForceStop(project, paths);

        Assert.Equal(1, n);                                           // 只杀匹配工程的
        Assert.Equal(new[] { 111 }, killed);
        Assert.True(File.Exists(paths.StopFlag));
        Assert.True(File.Exists(paths.StopIntent));                   // watchdog 见它不重启★
    }

    [Fact]
    public void ForceStop_NoMatch_ReturnsZero_StillWritesIntent()
    {
        var paths = new Paths("F:/p/P.uproject");
        Directory.CreateDirectory(paths.ConfigDir);
        var ctl = new EngineControl(new RecordingQuery(), _ => true);
        Assert.Equal(0, ctl.ForceStop("F:/p/P.uproject", paths));
        Assert.True(File.Exists(paths.StopIntent));
    }

    // ── VM：身份选项 + 筛选 + 新构造 ──
    private MainViewModel MakeVm(FakeFileSystem fs, Paths p,
        PipelineSnapshot? state = null, string? index = null,
        double stateAge = 0, double progressAge = 0)
    {
        var now = DateTime.UtcNow;
        if (state is not null) fs.SetFile(p.State,
            System.Text.Json.JsonSerializer.Serialize(state), now.AddSeconds(-stateAge));
        if (index is not null) fs.SetFile(p.IdentityIndex, index, now);
        var reader = new StateReader(fs);
        var launcher = new LauncherClient(new FakeRunner());
        var engine = new EngineControl(new RecordingQuery(), _ => true);
        return new MainViewModel(reader, launcher, p, fs, engine);
    }

    [Fact]
    public void Vm_IdentityOptions_MarksSmallAsSuspectEmpty()
    {
        var fs = new FakeFileSystem();
        var p = new Paths("proj/P.uproject");
        var idx = "{\"assets\":[" +
                  "{\"name\":\"big\",\"path\":\"/Game/big\",\"dir\":\"/Game\",\"modified\":\"x\",\"size_kb\":66210}," +
                  "{\"name\":\"tiny\",\"path\":\"/Game/tiny\",\"dir\":\"/Game\",\"modified\":\"x\",\"size_kb\":200}]," +
                  "\"dirs\":[\"/Game\"]}";
        var vm = MakeVm(fs, p, index: idx);
        vm.ReloadIdentityOptions();
        Assert.Equal(2, vm.IdentityOptions.Count);
        Assert.DoesNotContain("疑似空身份", vm.IdentityOptions[0].Display);   // 66MB 成型
        Assert.Contains("疑似空身份", vm.IdentityOptions[1].Display);        // 200KB 警示★
        Assert.Equal("/Game/big", vm.IdentityOptions[0].Path);
        // 显示文本要短（目录取末段）+ 带大小；完整路径在 Path（ToolTip 用）
        Assert.Equal("big · Game · 64.7 MB", vm.IdentityOptions[0].Display);
        Assert.Equal("tiny · Game · 200 KB（疑似空身份）", vm.IdentityOptions[1].Display);
    }

    [Fact]
    public void Vm_ConfirmAndClearBinding_WriteFile()
    {
        var fs = new FakeFileSystem();
        var p = new Paths("proj/P.uproject");
        var bindPath = Path.Combine(_tmp, "b.json");
        var vm = MakeVm(fs, p);
        vm.BindingsPath = bindPath;

        Assert.True(vm.ConfirmBinding("pm", "/Game/ID/ID_pm"));
        var store = new BindingStore(new RealFileSystem(), bindPath);
        Assert.Equal("/Game/ID/ID_pm", store.Load()["pm"]);

        Assert.True(vm.ClearBinding("pm"));
        Assert.False(store.Load().ContainsKey("pm"));

        Assert.False(vm.ConfirmBinding("pm", ""));                    // 未选身份 → 拒绝
        Assert.NotNull(vm.LastError);
    }

    private static PipelineSnapshot SampleWithEvents() => new()
    {
        State = "running",
        Events = new List<EventItem>
        {
            new() { T = "10:00", Lv = "失败", Tx = "taskA 崩了" },
            new() { T = "09:58", Lv = "待处理", Tx = "taskB 排队" },
            new() { T = "09:55", Lv = "完成", Tx = "taskC 交付" },
            new() { T = "09:50", Lv = "绑定", Tx = "身份就绪" },
        }
    };

    [Fact]
    public void Vm_EventFilter_ComputedImmediately()
    {
        var fs = new FakeFileSystem();
        var p = new Paths("proj/P.uproject");
        var vm = MakeVm(fs, p, state: SampleWithEvents());
        vm.Refresh(true, DateTime.UtcNow);

        Assert.Equal(4, vm.EventLines.Count);                          // All
        vm.Filter = EventFilter.Failed;
        Assert.Single(vm.EventLines);                                  // 立即生效（不等下轮★）
        Assert.Contains("失败", vm.EventLines[0]);
        vm.Filter = EventFilter.Pending;
        Assert.Equal(2, vm.EventLines.Count);                          // 待处理+绑定
    }

    [Fact]
    public void Vm_GroupMatrix_And_Eta()
    {
        var fs = new FakeFileSystem();
        var p = new Paths("proj/P.uproject");
        var snap = SampleWithEvents();
        snap.Queued = 3;
        snap.EtaSeconds = 4100;
        snap.IdentityGroups = new List<IdentityGroup>
        {
            new() { Dir = "pm/", Id = "ID_pm", Done = 2, Total = 3, Failed = 1, Pending = 1 }
        };
        var vm = MakeVm(fs, p, state: snap);
        vm.Refresh(true, DateTime.UtcNow);
        Assert.Contains("预计追平", vm.EtaText);
        var line = Assert.Single(vm.GroupMatrixLines);
        Assert.Contains("pm/ → ID_pm · 2/3 · 失败1 · 待1", line);
    }

    [Fact]
    public void Vm_NoQueue_EtaEmpty()
    {
        var fs = new FakeFileSystem();
        var p = new Paths("proj/P.uproject");
        var snap = SampleWithEvents();
        snap.Queued = 0;
        snap.EtaSeconds = 100;
        var vm = MakeVm(fs, p, state: snap);
        vm.Refresh(true, DateTime.UtcNow);
        Assert.Equal("", vm.EtaText);
    }
}
