// 严格测试：① 真实样本契约比对（防漂移★）② StateReader 解析/容错/心跳/派生状态
// ③ 去重 ④ LauncherClient 参数 ⑤ ViewModel 状态与命令

using System;
using System.IO;
using System.Linq;
using MhsPipeline.Core;
using Xunit;

namespace MhsPipeline.Tests;

public class CoreTests
{
    private static readonly string DataDir = Path.Combine(
        Path.GetDirectoryName(typeof(CoreTests).Assembly.Location) ?? ".", "testdata");

    private static string Real(string name) => File.ReadAllText(Path.Combine(DataDir, name));

    // ── 1) 契约比对：真实 stream_state.json 能被完整解析（字段齐全） ──
    [Fact]
    public void Contract_StreamState_Parses_AllFields()
    {
        var s = Jsonx.TryParse<PipelineSnapshot>(Real("stream_state.json"));
        Assert.NotNull(s);
        Assert.False(string.IsNullOrEmpty(s!.State));
        Assert.True(s.Synced >= 0 && s.Delivered >= 0 && s.Failed >= 0 && s.Queued >= 0);
        Assert.NotNull(s.StageCounts);
        Assert.NotNull(s.IdentityGroups);
        Assert.NotNull(s.Events);
        Assert.NotNull(s.TasksDetail);
        Assert.NotNull(s.TasksDetail!.Loading);
        Assert.NotNull(s.TasksDetail.Active);
        Assert.NotNull(s.TasksDetail.Done);
        Assert.NotNull(s.TasksDetail.Failed);
        Assert.NotNull(s.TasksDetail.Imported);      // 新增列必须存在（历史 bug：ID 误报）
        Assert.False(string.IsNullOrEmpty(s.UpdatedAt));

        foreach (var g in s.IdentityGroups)
        {
            Assert.False(string.IsNullOrEmpty(g.Dir));
            Assert.False(string.IsNullOrEmpty(g.Id));
        }
        foreach (var e in s.Events)
            Assert.False(string.IsNullOrEmpty(e.Lv));
    }

    [Fact]
    public void Contract_Queue_Progress_Status_Identity_Parse()
    {
        var q = Jsonx.TryParse<QueueSnapshot>(Real("queue_state.json"));
        Assert.NotNull(q);
        Assert.NotEmpty(q!.Tasks);
        foreach (var t in q.Tasks)
        {
            Assert.False(string.IsNullOrEmpty(t.Key));
            Assert.False(string.IsNullOrEmpty(t.Status));
        }

        var p = Jsonx.TryParse<ProgressSnapshot>(Real("progress.json"));
        Assert.NotNull(p);

        var st = Jsonx.TryParse<EngineStatus>(Real("pipeline.status.json"));
        Assert.NotNull(st);

        var idx = Jsonx.TryParse<IdentityIndex>(Real("identity_assets.json"));
        Assert.NotNull(idx);
        Assert.NotNull(idx!.Assets);
        foreach (var a in idx.Assets)
        {
            Assert.False(string.IsNullOrEmpty(a.Path));
            Assert.False(string.IsNullOrEmpty(a.Name));
            Assert.True(a.SizeKb >= 0);
        }
    }

    [Fact]
    public void Contract_MissingOrBrokenJson_ReturnsNull_NoThrow()
    {
        Assert.Null(Jsonx.TryParse<PipelineSnapshot>(null));
        Assert.Null(Jsonx.TryParse<PipelineSnapshot>(""));
        Assert.Null(Jsonx.TryParse<PipelineSnapshot>("{broken"));
        Assert.Null(Jsonx.TryParse<QueueSnapshot>("[]"));
    }

    // ── 2) StateReader ──
    private static (FakeFileSystem fs, Paths p) Env(string? state = null, string? queue = null,
                                                    string? progress = null, double stateAge = 0,
                                                    double progressAge = 0)
    {
        var fs = new FakeFileSystem();
        var now = DateTime.UtcNow;
        var p = new Paths(Path.Combine("proj", "P.uproject"));
        if (state is not null) fs.SetFile(p.State, state, now.AddSeconds(-stateAge));
        if (queue is not null) fs.SetFile(p.Queue, queue, now.AddSeconds(-1));
        if (progress is not null) fs.SetFile(p.Progress, progress, now.AddSeconds(-progressAge));
        return (fs, p);
    }

    private const string QueueOne = "{\"tasks\":[{\"key\":\"a\",\"status\":\"solving\"}]}";
    private const string QueueEmpty = "{\"tasks\":[{\"key\":\"a\",\"status\":\"done\"}]}";

    [Fact]
    public void Reader_Heartbeat_UsesNewerOfStateAndProgress()
    {
        var (fs, p) = Env(state: "{\"state\":\"idle\"}", progress: "{\"state\":\"solving\"}",
                          stateAge: 10, progressAge: 2);
        var age = new StateReader(fs).HeartbeatAgeSeconds(p, DateTime.UtcNow);
        Assert.NotNull(age);
        Assert.InRange(age!.Value, 1.0, 4.0);      // 取较新的 progress(2s)
    }

    [Fact]
    public void Reader_Heartbeat_Null_WhenNoFiles()
    {
        var (fs, p) = Env();
        Assert.Null(new StateReader(fs).HeartbeatAgeSeconds(p, DateTime.UtcNow));
    }

    [Fact]
    public void Reader_Derive_AliveFresh_Healthy()
    {
        var (fs, p) = Env(state: "{\"state\":\"running\"}", queue: QueueEmpty,
                          progress: "{\"state\":\"solving\"}", progressAge: 1);
        Assert.Equal("healthy", new StateReader(fs).DeriveState(p, true, DateTime.UtcNow));
    }

    [Fact]
    public void Reader_Derive_AliveStale_WithActive_Stalled()
    {
        // state 与 progress 都要变旧（心跳取较新者，只旧一个不算陈腐）
        var (fs, p) = Env(state: "{\"state\":\"running\"}", queue: QueueOne,
                          progress: "{\"state\":\"solving\"}", stateAge: 5000, progressAge: 5000);
        Assert.Equal("stalled", new StateReader(fs).DeriveState(p, true, DateTime.UtcNow));
    }

    [Fact]
    public void Reader_Derive_AliveStale_NoActive_Idle_NotFault()
    {
        var (fs, p) = Env(state: "{\"state\":\"idle\"}", queue: QueueEmpty,
                          progress: "{\"state\":\"solving\"}", stateAge: 5000, progressAge: 5000);
        Assert.Equal("idle", new StateReader(fs).DeriveState(p, true, DateTime.UtcNow));
    }

    [Fact]
    public void Reader_Derive_DeadWithActive_IsDead_UnlessIntent()
    {
        var (fs, p) = Env(state: "{\"state\":\"idle\"}", queue: QueueOne);
        Assert.Equal("dead", new StateReader(fs).DeriveState(p, false, DateTime.UtcNow));
        fs.SetFile(p.StopIntent, "1");
        Assert.Equal("paused", new StateReader(fs).DeriveState(p, false, DateTime.UtcNow));
    }

    [Fact]
    public void Reader_Derive_DeadNoActive_Stopped()
    {
        var (fs, p) = Env(queue: QueueEmpty);
        Assert.Equal("stopped", new StateReader(fs).DeriveState(p, false, DateTime.UtcNow));
    }

    [Fact]
    public void Reader_BrokenJson_ActiveCountZero_NoThrow()
    {
        var (fs, p) = Env(state: "{broken", queue: "{broken", progress: "{broken");
        var r = new StateReader(fs);
        Assert.Equal(0, r.ActiveTaskCount(r.ReadQueue(p)));
        Assert.Null(r.ReadState(p));
    }

    // ── 3) 去重 ──
    [Fact]
    public void Dedup_SameContentSkipped_DifferentRenders()
    {
        var d = new RenderDeduplicator();
        Assert.True(d.ShouldRender("A"));
        Assert.False(d.ShouldRender("A"));
        Assert.True(d.ShouldRender("B"));
        Assert.False(d.ShouldRender("B"));
    }

    // ── 4) LauncherClient 参数 ──
    [Fact]
    public void Launcher_Start_IsDetached_And_PassesConfig()
    {
        var runner = new FakeRunner();
        var cli = new LauncherClient(runner, "launcher.py") { Python = "python" };
        cli.Start("C:\\cfg\\pipeline.config.json");
        var call = Assert.Single(runner.Calls);
        Assert.False(call.Wait);                        // 启动：分离（不阻塞 UI）
        Assert.Contains("start --config", call.Args);
        Assert.Contains("pipeline.config.json", call.Args);
        // 分离命令必须经 cmd 转发 + 输出落日志（GUI 不持管道：管道无人读 + Dispose 关闭句柄 →
        // 子进程打印即死 → [启动] 写完 job 就没了，watchdog/status 永不出现）
        Assert.Equal("cmd.exe", call.File);
        Assert.Contains(">>", call.Args);
        Assert.Contains(".gui.log", call.Args);
    }

    [Fact]
    public void Launcher_Pause_And_NewProject_Args()
    {
        var runner = new FakeRunner();
        var cli = new LauncherClient(runner, "launcher.py") { Python = "python" };
        cli.Pause("cfg.json");
        Assert.Contains("pause --config", runner.Calls[0].Args);
        Assert.True(runner.Calls[0].Wait);
        cli.NewProject("MH_Line01", "D:\\projects");
        Assert.Contains("new --name", runner.Calls[1].Args);
        Assert.Contains("MH_Line01", runner.Calls[1].Args);
    }

    // ── 5) ViewModel ──
    private MainViewModel MakeVm(FakeFileSystem fs, Paths p) =>
        new(new StateReader(fs), new LauncherClient(new FakeRunner()), p, fs,
            new EngineControl(new FakeProcessQuery(), _ => true));

    [Fact]
    public void ViewModel_NoData_ShowsWaiting()
    {
        var (fs, p) = Env();
        var vm = MakeVm(fs, p);
        vm.Refresh(false, DateTime.UtcNow);
        Assert.Equal("等待数据", vm.StatusLine);
        Assert.False(vm.HasData);
        Assert.True(vm.IsIdleEmpty);
        // 年龄必须有界（曾显示 "223910500m前" 这种荒谬值）；无活跃任务 → 不显示进度卡
        Assert.True(vm.DataAgeText is "无数据" or "陈旧数据", vm.DataAgeText);
        Assert.False(vm.HasProgress);
    }

    [Fact]
    public void ViewModel_RealSample_StatusLineAndIdentityBand()
    {
        var fs = new FakeFileSystem();
        var p = new Paths(Path.Combine("proj", "P.uproject"));
        var now = DateTime.UtcNow;
        fs.SetFile(p.State, Real("stream_state.json"), now);
        fs.SetFile(p.Queue, Real("queue_state.json"), now);
        fs.SetFile(p.Progress, Real("progress.json"), now);

        var vm = MakeVm(fs, p);
        Assert.True(vm.Refresh(true, now));                  // 首次渲染
        Assert.False(vm.Refresh(true, now));                 // 内容未变 → 跳过重绘★
        Assert.True(vm.HasData);
        Assert.Contains("交付", vm.StatusLine);
        Assert.Contains("→", vm.IdentityBand);
        Assert.Equal("运行中", vm.EngineStateText);
    }

    [Fact]
    public void ViewModel_PausedState_ShownInStatus()
    {
        var (fs, p) = Env(state: Real("stream_state.json"), queue: Real("queue_state.json"),
                          progress: Real("progress.json"));
        fs.SetFile(p.StopIntent, "1");
        var vm = MakeVm(fs, p);
        vm.Refresh(false, DateTime.UtcNow);
        Assert.Equal("paused", vm.EngineState);
        Assert.Equal("暂停中", vm.EngineStateText);
        Assert.Contains("暂停中", vm.StatusLine);
    }

    [Fact]
    public void ViewModel_Commands_CallLauncher()
    {
        var (fs, p) = Env();
        var runner = new FakeRunner();
        var vm = new MainViewModel(new StateReader(fs), new LauncherClient(runner, "launcher.py"), p,
            fs, new EngineControl(new FakeProcessQuery(), _ => true));
        vm.Start();
        vm.Pause();
        vm.NewProject("X", "D:\\p");
        Assert.Equal(3, runner.Calls.Count);
        Assert.Contains("start", runner.Calls[0].Args);
        Assert.Contains("pause", runner.Calls[1].Args);
        Assert.Contains("new --name", runner.Calls[2].Args);
    }

    [Fact]
    public void ViewModel_FormatHelpers()
    {
        Assert.Equal("0s", MainViewModel.FmtDur(0));
        Assert.Equal("45s", MainViewModel.FmtDur(45));
        Assert.Equal("12m05s", MainViewModel.FmtDur(725));
        // 两套词表都映射（progress.json 单数 / 队列进行时）——曾漏后者，处理中行显示原始 "importing"
        Assert.Equal("深度解算中", MainViewModel.StageCn("solving"));
        Assert.Equal("导入素材中", MainViewModel.StageCn("import"));
        Assert.Equal("导入素材中", MainViewModel.StageCn("importing"));
        Assert.Equal("导出中", MainViewModel.StageCn("exporting"));
        Assert.Equal("排队中", MainViewModel.StageCn("pending"));
    }

    [Fact]
    public void Progress_Import_UsesVolumeEstimate_NotRawZero()
    {
        // 导入阶段引擎不给真实百分比（原始 percent=0）→ 按体积估算时长折算
        var fs = new FakeFileSystem();
        var p = new Paths("proj/P.uproject");
        var prog = new ProgressSnapshot
        {
            AssetName = "[导入] take_a",
            State = "import",
            Percent = 0,                       // 引擎原始值恒 0
            ElapsedSeconds = 100,
            PassCount = 1,
        };
        var state = new PipelineSnapshot { ImportEstimateSeconds = 200 };
        var vm = MakeVm(fs, p);
        typeof(MainViewModel)
            .GetMethod("BuildProgress", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!
            .Invoke(vm, new object?[] { prog, state });
        Assert.True(vm.HasProgress);
        Assert.Contains("50%", vm.ProgressTitle);                    // 100/200 → 50%
        Assert.Contains("估算", vm.ProgressTitle);
        Assert.Equal(50, vm.ProgressPercent, 0);
        Assert.DoesNotContain("Pass", vm.ProgressDetail);            // 1/1 噪音不显示
    }

    [Fact]
    public void ViewModel_SegmentedStatus_IdentityParts()
    {
        var fs = new FakeFileSystem();
        var p = new Paths("proj/P.uproject");
        var now = DateTime.UtcNow;
        var snap = System.Text.Json.JsonSerializer.Deserialize<PipelineSnapshot>(Real("stream_state.json"));
        fs.SetFile(p.State, System.Text.Json.JsonSerializer.Serialize(snap), now);
        fs.SetFile(p.Queue, Real("queue_state.json"), now);
        fs.SetFile(p.Progress, Real("progress.json"), now);
        var vm = MakeVm(fs, p);
        vm.Refresh(true, now);

        // 分段着色（V6 原型）：状态词/计数/身份带各自独立
        Assert.Equal("监听中", vm.StateText);
        Assert.Contains("队列", vm.CountsLine);
        Assert.Contains("交付", vm.CountsLine);
        // 进度卡（原型布局）：标题带百分比 / 明细行灰 / HasProgress
        if (vm.HasProgress)
        {
            Assert.Contains("%", vm.ProgressTitle);
            Assert.False(string.IsNullOrEmpty(vm.ProgressTitle));
        }
        Assert.True(vm.HasIdentity);
        Assert.Equal("am/", vm.IdentityGroupText);                     // 组优先（此前根组优先的修复生效）
        Assert.False(string.IsNullOrEmpty(vm.IdentityPathText));
        // StatusLine 保持兼容（测试与可能的外部消费）
        Assert.Contains("交付", vm.StatusLine);
    }

    // ── 工程选择：清单记忆 / 扫描 / 切换 ──
    [Fact]
    public void ProjectStore_Remembers_And_DropsMissing()
    {
        var dir = Path.Combine(Path.GetTempPath(), "mhs_pt_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var store = Path.Combine(dir, "projects.json");
            var a = Path.Combine(dir, "A.uproject");
            var b = Path.Combine(dir, "B.uproject");
            File.WriteAllText(a, "{}");
            File.WriteAllText(b, "{}");

            ProjectStore.Add(store, a);
            ProjectStore.Add(store, b);
            ProjectStore.Add(store, a);                      // 重复 → 去重且置顶
            var list = ProjectStore.Load(store);
            Assert.Equal(2, list.Count);
            Assert.Equal(a, list[0]);                        // 最近用的在前

            File.Delete(b);                                  // 消失的工程不展示
            Assert.Single(ProjectStore.Load(store));
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void ProjectDiscovery_Scan_FindsUproject()
    {
        var root = Path.Combine(Path.GetTempPath(), "mhs_scan_" + Guid.NewGuid().ToString("N"));
        var deep = Path.Combine(root, "a", "b", "c");
        Directory.CreateDirectory(deep);
        Directory.CreateDirectory(Path.Combine(root, "Intermediate"));
        try
        {
            File.WriteAllText(Path.Combine(root, "Top.uproject"), "{}");
            File.WriteAllText(Path.Combine(deep, "Deep.uproject"), "{}");
            File.WriteAllText(Path.Combine(root, "Intermediate", "Skip.uproject"), "{}");

            var found = ProjectDiscovery.Scan(root, 3);
            Assert.Contains(found, p => p.EndsWith("Top.uproject"));
            Assert.Contains(found, p => p.EndsWith("Deep.uproject"));
            Assert.DoesNotContain(found, p => p.Contains("Intermediate"));   // 中间目录跳过
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void ViewModel_SwitchProject_RebuildsPaths()
    {
        var dir = Path.Combine(Path.GetTempPath(), "mhs_sw_" + Guid.NewGuid().ToString("N"));
        var p1 = Path.Combine(dir, "P1"); var p2 = Path.Combine(dir, "P2");
        Directory.CreateDirectory(p1); Directory.CreateDirectory(p2);
        try
        {
            var cfgPath = Path.Combine(dir, "pipeline.config.json");
            var up1 = Path.Combine(p1, "P1.uproject"); File.WriteAllText(up1, "{}");
            var up2 = Path.Combine(p2, "P2.uproject"); File.WriteAllText(up2, "{}");
            var fs = new FakeFileSystem();
            var vm = new MainViewModel(new StateReader(fs), new LauncherClient(new FakeRunner()),
                                       new Paths(up1), fs, new EngineControl(new FakeProcessQuery(), _ => true))
            { ConfigPath = cfgPath, Config = new PipelineConfig { Project = up1 } };

            Assert.False(vm.SwitchProject(Path.Combine(dir, "missing.uproject")));   // 不存在 → 拒绝
            Assert.NotNull(vm.LastError);

            Assert.True(vm.SwitchProject(up2));
            Assert.Equal(up2, vm.Config!.Project);
            Assert.StartsWith(p2, vm.Paths.ConfigDir);                              // 路径随工程重建
            Assert.StartsWith(p2, vm.BindingsPath);
            Assert.True(File.Exists(cfgPath));                                      // 配置已落盘
            Assert.Contains("P2", File.ReadAllText(cfgPath));
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void ViewModel_GroupMatrix_FallsBackToBindings_WhenNoState()
    {
        var dir = Path.Combine(Path.GetTempPath(), "mhs_gm_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var bind = Path.Combine(dir, "stream_bindings.json");
            File.WriteAllText(bind, "{\"bindings\":{\"am\":\"/Game/CaptureManager/ID4/am_001\",\"\":\"/Game/ID/root_x\"}}");
            var fs = new FakeFileSystem();
            var p = new Paths(Path.Combine(dir, "P.uproject"));
            var vm = MakeVm(fs, p);
            vm.BindingsPath = bind;
            vm.Refresh(false, DateTime.UtcNow);            // 无状态文件（引擎未启动）
            Assert.Contains(vm.GroupMatrixLines, l => l.Contains("am/") && l.Contains("ID4/am_001"));
            Assert.Contains(vm.GroupMatrixLines, l => l.Contains("(根)") && l.Contains("root_x"));
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void GroupOptions_AreTyped_NotTuples()
    {
        // 元组会让 XAML {Binding Display}/SelectedValuePath 静默失败（下拉空白）→ 必须是正式类型
        var inbox = Path.Combine(Path.GetTempPath(), "mhs_go_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(inbox, "am"));
        Directory.CreateDirectory(Path.Combine(inbox, "_id"));
        try
        {
            var opts = GroupOptions.List(new RealFileSystem(), inbox);
            var am = opts.First(o => o.Display == "am/");
            Assert.Equal("am", am.Value);
            var t = am.GetType();
            Assert.NotNull(t.GetProperty("Display"));
            Assert.NotNull(t.GetProperty("Value"));
            Assert.DoesNotContain("Item1", t.GetProperties().Select(x => x.Name));
        }
        finally { Directory.Delete(inbox, true); }
    }

    [Fact]
    public void ViewModel_SwitchProject_AdoptsOwnConfigPaths()
    {
        // 切到"自带 pipeline.config.json"的工程 → 路径随之切换（否则工程=A、路径=B 的错配）
        var dir = Path.Combine(Path.GetTempPath(), "mhs_sw2_" + Guid.NewGuid().ToString("N"));
        var p1 = Path.Combine(dir, "P1"); var p2 = Path.Combine(dir, "P2");
        Directory.CreateDirectory(p1); Directory.CreateDirectory(p2);
        try
        {
            var up1 = Path.Combine(p1, "P1.uproject"); File.WriteAllText(up1, "{}");
            var up2 = Path.Combine(p2, "P2.uproject"); File.WriteAllText(up2, "{}");
            File.WriteAllText(Path.Combine(p2, "pipeline.config.json"), """
                {"instance":"P2","inbox":"E:/x/P2_inbox","fbx_output":"E:/x/P2_out",
                 "import_root":"/Game/CaptureManager/AutoP2","identity_import_root":"/Game/CaptureManager/ID"}
                """);
            var cfgPath = Path.Combine(dir, "pipeline.config.json");
            var fs = new FakeFileSystem();
            var vm = new MainViewModel(new StateReader(fs), new LauncherClient(new FakeRunner()),
                                       new Paths(up1), fs, new EngineControl(new FakeProcessQuery(), _ => true))
            {
                ConfigPath = cfgPath,
                Config = new PipelineConfig
                {
                    Project = up1, Inbox = "E:/x/P1_inbox",
                    ImportRoot = "/Game/CaptureManager/AutoP1", FbxOutput = "E:/x/P1_out"
                }
            };
            Assert.True(vm.SwitchProject(up2));
            Assert.Equal("E:/x/P2_inbox", vm.Config!.Inbox);                       // 采用自带配置
            Assert.Equal("/Game/CaptureManager/AutoP2", vm.Config.ImportRoot);

            // 无自带配置的工程 → 保持原路径（向后兼容：MH_H 这类老工程）
            var p3 = Path.Combine(dir, "P3"); Directory.CreateDirectory(p3);
            var up3 = Path.Combine(p3, "P3.uproject"); File.WriteAllText(up3, "{}");
            Assert.True(vm.SwitchProject(up3));
            Assert.Equal("E:/x/P2_inbox", vm.Config.Inbox);                        // 不变
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void InboxScan_Arrived_FindsUnknownTakes_Only()
    {
        var inbox = Path.Combine(Path.GetTempPath(), "mhs_scan_inbox_" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(Path.Combine(inbox, "am", "take_a")); File.WriteAllText(Path.Combine(inbox, "am", "take_a", "take.json"), "{}");
            Directory.CreateDirectory(Path.Combine(inbox, "am", "take_b")); File.WriteAllText(Path.Combine(inbox, "am", "take_b", "take.json"), "{}");
            Directory.CreateDirectory(Path.Combine(inbox, "am", "junk"));                    // 无 take.json → 忽略
            Directory.CreateDirectory(Path.Combine(inbox, "_id", "rom_x")); File.WriteAllText(Path.Combine(inbox, "_id", "rom_x", "take.json"), "{}");
            Directory.CreateDirectory(Path.Combine(inbox, "root_take")); File.WriteAllText(Path.Combine(inbox, "root_take", "take.json"), "{}");

            var lines = InboxScan.Arrived(inbox, new[] { "am/take_a" });                     // 已知的不显示
            Assert.Contains(lines, l => l.Contains("[盘] am/take_b"));
            Assert.Contains(lines, l => l.Contains("[盘] _id/rom_x"));
            Assert.Contains(lines, l => l.Contains("[盘] root_take"));
            Assert.DoesNotContain(lines, l => l.Contains("am/take_a"));
            Assert.DoesNotContain(lines, l => l.Contains("junk"));

            // 文案区分：引擎未就绪（新工程首次启动装依赖中）≠ 排队等当前任务
            var notReady = InboxScan.Arrived(inbox, new[] { "am/take_a" }, engineReady: false);
            Assert.Contains(notReady, l => l.Contains("等待引擎就绪") && l.Contains("Python"));
            Assert.Contains(lines, l => l.Contains("等待接管"));
        }
        finally { Directory.Delete(inbox, true); }
    }

    [Fact]
    public void ViewModel_Paused_StateText_Amber()
    {
        var (fs, p) = Env(state: Real("stream_state.json"), queue: Real("queue_state.json"),
                          progress: Real("progress.json"));
        fs.SetFile(p.StopIntent, "1");
        var vm = MakeVm(fs, p);
        vm.Refresh(false, DateTime.UtcNow);
        Assert.Equal("暂停中", vm.StateText);                          // 状态词分段（琥珀由 UI 渲染）
    }
}
