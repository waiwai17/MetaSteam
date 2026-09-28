// 主视图模型：UI 的全部"判断与文案"（可单测），XAML 只绑定显示。
// 纪律：界面只留状态与动作；只写配置与绑定，不写队列。

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace MhsPipeline.Core;

public enum EventFilter { All, Failed, Pending }

public sealed class MainViewModel
{
    private readonly StateReader _reader;
    private readonly LauncherClient _launcher;
    private readonly RenderDeduplicator _dedup = new();
    private readonly IFileSystem _fs;
    private readonly EngineControl _engine;

    public MainViewModel(StateReader reader, LauncherClient launcher, Paths paths,
                         IFileSystem fs, EngineControl engine)
    {
        _reader = reader;
        _launcher = launcher;
        Paths = paths;
        _fs = fs;
        _engine = engine;
    }

    public Paths Paths { get; private set; }
    public string ConfigPath { get; set; } = "";
    public PipelineConfig? Config { get; set; }
    public string BindingsPath { get; set; } = "";

    // ── 显示属性 ──
    public string EngineState { get; private set; } = "unknown";
    public string EngineStateText { get; private set; } = "未运行";
    // 分段着色（按 V6 原型：状态词带色 / 计数灰 / ETA 蓝）——StatusLine 保留给测试与兼容
    public string StateText { get; private set; } = "未运行";        // 绿=监听中 琥珀=暂停中 红=异常
    public string CountsLine { get; private set; } = "等待数据";      // 灰：队列/交付/失败
    public string StatusLine { get; private set; } = "等待数据";
    public string EtaText { get; private set; } = "";
    public string IdentityBand { get; private set; } = "";
    public bool HasIdentity { get; private set; }
    public string IdentityGroupText { get; private set; } = "";      // "pm/"（白）
    public string IdentityPathText { get; private set; } = "";       // 资产路径（白）
    public string IdentityTailText { get; private set; } = "";       // 来源/建议（灰）
    public string ProgressText { get; private set; } = "";
    public bool HasProgress { get; private set; }
    public string ProgressTitle { get; private set; } = "";     // "pm/xxx 深度解算 83%"（原型标题行）
    public string ProgressDetail { get; private set; } = "";    // "Pass 2/3 · …帧 · 已跑 … · 剩余约 …"（灰明细）
    public double ProgressPercent { get; private set; }
    public bool HasData { get; private set; }
    public string DataAgeText { get; private set; } = "";
    public string TitleInfo { get; private set; } = "";

    public int LoadingCount => LoadingLines.Count;
    public int ActiveCount => ActiveLines.Count;
    public int FailedCount => FailedLines.Count;
    public int DoneCount => DoneLines.Count;

    public List<string> LoadingLines { get; private set; } = new();
    public List<string> ActiveLines { get; private set; } = new();
    public List<string> FailedLines { get; private set; } = new();
    public List<string> DoneLines { get; private set; } = new();
    public List<string> GroupMatrixLines { get; private set; } = new();
    private List<EventItem> _events = new();

    /// <summary>按当前筛选即时计算（切换筛选不需要等下一轮刷新）。</summary>
    public List<string> EventLines => Filter switch
    {
        EventFilter.Failed => _events.Where(e => e.Lv == "失败").Select(FmtEvent).ToList(),
        EventFilter.Pending => _events.Where(e => e.Lv is "待处理" or "绑定").Select(FmtEvent).ToList(),
        _ => _events.Select(FmtEvent).ToList()
    };

    public bool HasLoading => LoadingLines.Count > 0;
    public bool HasActive => ActiveLines.Count > 0;
    public bool HasFailed => FailedLines.Count > 0;
    public bool HasDone => DoneLines.Count > 0;
    public bool IsIdleEmpty => !HasLoading && !HasActive && !HasFailed && !HasDone;
    public string EmptyText => IsIdleEmpty ? "空闲 · 素材拷入热文件夹后自动开始" : "";

    public string? LastError { get; private set; }
    public string? LastAction { get; private set; }

    public EventFilter Filter { get; set; } = EventFilter.All;

    // ── 身份资产选项（来自 identity_assets.json） ──
    public List<IdentityOption> IdentityOptions { get; private set; } = new();

    public sealed record IdentityOption(string Display, string Path);

    public void ReloadIdentityOptions()
    {
        var idx = _reader.ReadIdentityIndex(Paths);
        IdentityOptions = (idx?.Assets ?? new List<IdentityAsset>())
            .Select(a => new IdentityOption(
                // 目录只取末段（如 /Game/CaptureManager/ID4 → ID4）：下拉框窄，全路径读不清；
                // 完整路径放 ToolTip（XAML ItemTemplate 绑定 Path）
                $"{a.Name} · {ShortDir(a.Dir)} · {SizeText(a.SizeKb)}"
                    + (a.SizeKb > 10000 ? "" : "（疑似空身份）"),
                a.Path))
            .ToList();
    }

    private static string ShortDir(string dir)
    {
        if (string.IsNullOrEmpty(dir)) return "";
        var seg = dir.TrimEnd('/').Split('/');
        return seg[^1];
    }

    private static string SizeText(long kb) => kb > 1024 ? $"{kb / 1024.0:0.#} MB" : $"{kb} KB";

    // ── 轮询刷新 ──
    public bool Refresh(bool engineAlive, DateTime nowUtc)
    {
        LastError = null;
        EngineState = _reader.DeriveState(Paths, engineAlive, nowUtc);
        EngineStateText = EngineState switch
        {
            "healthy" => "运行中",
            "idle" => "运行中 · 无任务",
            "stalled" => "无响应",
            "dead" => "已中断",
            "stopped" => "已停止",
            "paused" => "暂停中",
            _ => "未运行"
        };

        var state = SafeRead(() => _reader.ReadState(Paths), "状态");
        var queue = SafeRead(() => _reader.ReadQueue(Paths), "队列");
        var progress = SafeRead(() => _reader.ReadProgress(Paths), "进度");

        HasData = state is not null || queue is not null || progress is not null;
        var age = _reader.HeartbeatAgeSeconds(Paths, nowUtc);
        // 年龄显示要诚实且有界：读不到 → 无数据；超过 6 小时的数据没有"实时"意义
        // （曾直接按分钟格式化 → 出现 "223910500m前" 这种荒谬值）
        DataAgeText = age is null ? "无数据"
            : age.Value > 6 * 3600 ? "陈旧数据"
            : $"{FmtAge(age.Value)}前";

        StateText = EngineState switch
        {
            "healthy" => "监听中",
            "idle" => "监听中 · 无任务",
            "paused" => "暂停中",
            "stalled" => "无响应",
            "dead" => "已中断",
            "stopped" => "已停止",
            _ => "未运行"
        };
        CountsLine = HasData
            ? $"队列 {state?.Queued ?? 0} · 交付 {state?.Delivered ?? 0} · 失败 {state?.Failed ?? 0}"
            : "等待数据";

        StatusLine = HasData
            ? $"队列 {state?.Queued ?? 0} · 交付 {state?.Delivered ?? 0} · 失败 {state?.Failed ?? 0}"
            : "等待数据";
        if (EngineState == "paused") StatusLine += " · 暂停中";
        if (EngineState == "stalled") StatusLine += " · 底层无响应";
        if (state?.PendingBinding > 0) StatusLine += $" · 待绑定 {state.PendingBinding}";

        EtaText = BuildEta(state);
        IdentityBand = BuildIdentityBand(state);
        // 身份带分段（着色由 UI 层渲染）
        if (state?.Identity is not null)
        {
            HasIdentity = true;
            var id = state.Identity;
            var group = id.Group;
            var asset = id.Asset;
            var source = id.Source;
            var following = false;
            // 关键：身份带要跟随**当前任务所属组**的绑定身份——不能在解算 pm 时显示 am 的身份
            // （实测：处理中 pm/…，顶部却显示 am/ → 误导）。绑定了哪一组就显示哪一组。
            if (state.TasksDetail?.Active is { Count: > 0 } && id.Bound is { Count: > 0 })
            {
                var act = state.TasksDetail.Active[0];
                if (id.Bound.TryGetValue(act.Group ?? "", out var actAsset) && !string.IsNullOrEmpty(actAsset))
                {
                    group = act.Group ?? "";
                    asset = actAsset;
                    source = System.IO.Path.GetFileName(actAsset.Replace('\\', '/'));
                    following = true;
                }
            }
            IdentityGroupText = (string.IsNullOrEmpty(group) ? "(根)" : group) + "/";
            IdentityPathText = asset;
            // 尾部：绑定总览 + 来源（只在与该身份同源时出现）+ 当前任务标注
            var tail = new List<string>();
            if (id.BoundCount > 1 && id.Bound is not null)
            {
                var names = id.Bound.Keys.Select(g => string.IsNullOrEmpty(g) ? "(根)" : g + "/");
                tail.Add($"已绑定 {id.BoundCount} 组（{string.Join(" ", names)}）");
            }
            if (following) tail.Add("当前任务所用");
            if (!string.IsNullOrEmpty(source)) tail.Add($"来源 {source}");
            if (!following && !string.IsNullOrEmpty(id.SuggestedGroup) && id.SuggestedGroup != group)
                tail.Add($"ROM 建议 {id.SuggestedGroup}/");
            IdentityTailText = string.Join(" · ", tail);
        }
        else
        {
            HasIdentity = false;
            IdentityGroupText = IdentityPathText = IdentityTailText = "";
        }
        TitleInfo = $"{Config?.Instance ?? "-"} · {EngineStateText}" +
                    (Config is { WebPort: > 0 } ? $" · Web {Config.WebPort}" : "");
        BuildProgress(progress, state);
        BuildLists(state);

        var sig = string.Join("|", EngineState, StatusLine, EtaText, IdentityBand,
            IdentityGroupText, IdentityPathText, IdentityTailText, HasIdentity, ProgressText,
            ProgressTitle, ProgressDetail, HasProgress,
            LoadingCount, ActiveCount, FailedCount, DoneCount,
            string.Join(",", LoadingLines), string.Join(",", ActiveLines),
            string.Join(",", FailedLines), string.Join(",", DoneLines),
            string.Join(",", EventLines), string.Join(",", GroupMatrixLines));
        return _dedup.ShouldRender(sig);
    }

    private static string BuildEta(PipelineSnapshot? s)
    {
        if (s is null || s.Queued <= 0 || s.EtaSeconds is null || s.EtaSeconds < 0) return "";
        var done = DateTime.Now.AddSeconds(s.EtaSeconds.Value);
        return $"预计追平 {done:HH:mm}（约 {FmtDur(s.EtaSeconds.Value)}）";
    }

    private static string BuildIdentityBand(PipelineSnapshot? s)
    {
        if (s?.Identity is null) { return ""; }
        return BuildIdentityBandParts(s.Identity);
    }

    private static string BuildIdentityBandParts(IdentityInfo id)
    {
        var grp = string.IsNullOrEmpty(id.Group) ? "(根)" : id.Group + "/";
        var band = $"{grp} → {id.Asset}";
        if (!string.IsNullOrEmpty(id.Source)) band += $" · 来源 {id.Source}";
        return band;
    }

    private void BuildProgress(ProgressSnapshot? p, PipelineSnapshot? state)
    {
        if (p is null || string.IsNullOrEmpty(p.State))
        {
            ProgressText = ""; ProgressPercent = 0;
            HasProgress = false; ProgressTitle = ""; ProgressDetail = "";
            return;
        }
        // 导入阶段：引擎不提供真实帧级百分比（C++ 转换进度只进日志）→ 按
        // "素材体积估算时长"（执行器注入 import_estimate_seconds，吞吐 ~1.5MB/s 标定）
        // 计算估算进度。曾直接显示原始 percent=0 → 进度条一直 0%（与 UE 控制台同口径）。
        var pct = Math.Clamp(p.Percent, 0, 100);
        var estNote = "";
        if (p.State == "import" && state?.ImportEstimateSeconds is > 0)
        {
            var est = state.ImportEstimateSeconds.Value;
            if (p.ElapsedSeconds <= est * 1.2)
            {
                pct = Math.Clamp(p.ElapsedSeconds / est * 100.0, 0.0, 95.0);
                estNote = $"（估算 · 预计 {FmtDur(est)}）";
            }
            else
            {
                // 超出预估（大素材/慢盘）：撤掉假百分比，诚实显示
                estNote = $"（超出预估 {FmtDur(est)}——大素材以实际完成为准）";
            }
        }
        ProgressPercent = pct;
        // 进度卡仅在任务活跃（导入/解算/导出进行中）时出现——
        // 终态残留（success/failed）挂着 "100%" 卡片与原型不符，也属 json 残留伪装实时
        var bActive = p.State is "import" or "solving" or "export";
        HasProgress = bActive;
        var pass = p.PassCount > 1 ? $"Pass {p.Pass}/{p.PassCount}" : "";   // 1/1 是噪音
        var frames = p.TotalFrames > 0 ? $"{p.CurrentFrame}/{p.TotalFrames} 帧" : "";
        var eta = p.EtaSeconds > 0 ? $"剩余约 {FmtDur(p.EtaSeconds)}" : "";
        ProgressTitle = !bActive ? ""
            : estNote.Contains("超出预估")
                ? $"{p.AssetName} {StageCn(p.State)} 已进行 {FmtDur(p.ElapsedSeconds)} {estNote}"
                : $"{p.AssetName} {StageCn(p.State)} {Math.Round(pct)}% {estNote}".TrimEnd();
        ProgressDetail = bActive
            ? string.Join(" · ", new[] { pass, frames,
                $"已跑 {FmtDur(p.ElapsedSeconds)}", eta }.Where(x => !string.IsNullOrEmpty(x)))
            : "";
        ProgressText = string.Join(" · ", new[] { p.AssetName, StageCn(p.State), pass, frames,
            $"已跑 {FmtDur(p.ElapsedSeconds)}", eta }.Where(x => !string.IsNullOrEmpty(x)));
    }

    public static string StageCn(string s) => s switch
    {
        // progress.json 用单数（import/solving/export），队列状态用进行时（importing/…）
        // ——曾只映射前者，处理中行显示原始 "importing"
        "solving" => "深度解算中",
        "import" or "importing" => "导入素材中",
        "export" or "exporting" => "导出中",
        "pending" => "排队中",
        "done" => "完成",
        "failed" => "失败",
        _ => s
    };

    private void BuildLists(PipelineSnapshot? s)
    {
        LoadingLines = new List<string>();
        ActiveLines = new List<string>();
        FailedLines = new List<string>();
        DoneLines = new List<string>();
        GroupMatrixLines = new List<string>();
        _events = new List<EventItem>();

        if (s?.TasksDetail is not null)
        {
            foreach (var r in s.TasksDetail.Loading) LoadingLines.Add($"{r.Key} · {r.Note}");
            foreach (var r in s.TasksDetail.Active) ActiveLines.Add($"{r.Key} · {StageCn(r.Status)}");
            foreach (var r in s.TasksDetail.Failed) FailedLines.Add($"{r.Key} · {r.Error}");
            foreach (var r in s.TasksDetail.Done) DoneLines.Add($"{r.Key} · {FmtDur(r.Duration ?? 0)}");
        }
        if (s is not null)
        {
            _events = s.Events ?? new List<EventItem>();
            foreach (var g in s.IdentityGroups)
            {
                var line = $"{g.Dir} → {g.Id} · {g.Done}/{g.Total}";
                if (g.Failed > 0) line += $" · 失败{g.Failed}";
                if (g.Pending > 0) line += $" · 待{g.Pending}";
                GroupMatrixLines.Add(line);
            }
        }
        // 引擎未启动（无状态文件）时，用绑定表兜底——否则该区块空白，
        // 用户看不到"哪个组绑了哪个身份"（这正是绑定前后的核对依据）
        if (GroupMatrixLines.Count == 0)
        {
            try
            {
                var binds = new BindingStore(new RealFileSystem(), BindingsPath).Load();
                foreach (var kv in binds.OrderBy(k => k.Key))
                {
                    var g = string.IsNullOrEmpty(kv.Key) ? "(根)" : kv.Key + "/";
                    GroupMatrixLines.Add($"{g} → {kv.Value} · 未开始");
                }
            }
            catch { /* 绑定表不可读 → 保持空 */ }
        }
        // 盘面兜底：引擎解算阻塞期 executor 不扫盘，state.json 看不到新拷入的素材
        // → GUI 自己扫 inbox，把"已到达未接管"的 take 显示出来（与 UE 控制台 [盘] 行同语义）
        var inbox = Config?.Inbox;
        if (!string.IsNullOrWhiteSpace(inbox))
        {
            var known = new List<string>();
            known.AddRange(LoadingLines.Select(l => l.Split(' ')[0]));
            if (s?.TasksDetail is not null)
            {
                known.AddRange(s.TasksDetail.Active.Select(a => a.Key));
                known.AddRange(s.TasksDetail.Done.Select(a => a.Key));
                known.AddRange(s.TasksDetail.Failed.Select(a => a.Key));
                known.AddRange(s.TasksDetail.Imported.Select(a => a.Key));
            }
            // engineReady = 引擎已写过 state（驱动在跑）；否则文案提示"等待引擎就绪"
            foreach (var line in InboxScan.Arrived(inbox, known, engineReady: s is not null))
                LoadingLines.Add(line);
        }
    }

    private static string FmtEvent(EventItem e) => $"{e.T} {e.Lv} {e.Tx}";

    public static string FmtDur(double seconds)
    {
        var ts = TimeSpan.FromSeconds(seconds);
        return ts.TotalHours >= 1
            ? $"{(int)ts.TotalHours}h{ts.Minutes:D2}m"
            : ts.TotalMinutes >= 1 ? $"{(int)ts.TotalMinutes}m{ts.Seconds:D2}s" : $"{(int)ts.TotalSeconds}s";
    }

    public static string FmtAge(double seconds) => seconds < 60 ? $"{seconds:0}s" : $"{seconds / 60:0}m";

    // ── 命令 ──
    public CommandResult? Start() => Safe(() => _launcher.Start(ConfigPath));
    public CommandResult? Pause() => Safe(() => _launcher.Pause(ConfigPath));
    public CommandResult? NewProject(string name, string dir) => Safe(() => _launcher.NewProject(name, dir));

    /// <summary>预检（launcher doctor）：只读检查配置/路径/绑定，不起引擎。返回 null 表示调用失败（见 LastError）。</summary>
    public CommandResult? Precheck() => Safe(() => _launcher.Doctor(ConfigPath));

    /// <summary>切换到已有工程：校验存在 → 落配置 → 重建路径。
    /// 路径必须重建：状态/队列/进度/绑定全部位于 &lt;工程&gt;/Saved/Config/MetaHumanSolver，
    /// 只改配置字符串而不换 Paths 会继续读旧工程的数据（自相矛盾）。</summary>
    public bool SwitchProject(string projectPath)
    {
        if (Config is null) { LastError = "配置未加载"; return false; }
        if (string.IsNullOrWhiteSpace(projectPath) || !File.Exists(projectPath))
        {
            LastError = "工程文件不存在：" + projectPath;
            return false;
        }
        if (!string.IsNullOrEmpty(Config.Project) &&
            Path.GetFullPath(Config.Project).Equals(Path.GetFullPath(projectPath), StringComparison.OrdinalIgnoreCase))
        {
            LastAction = "已是当前工程";
            return true;
        }
        try
        {
            Config.Project = projectPath;
            // 目标工程自带 pipeline.config.json（新建工程时生成）→ 采用它的路径配置。
            // 否则会出现"工程=A、路径仍是 B"的错配：实测切到 MH_Line01 后，面板仍写着
            // MH_H 的 ID4/AutoD1，而浏览框按新工程算出 Content\CaptureManager\AutoMH_Line01
            // —— 层级对不上（用户实测）。
            var ownDir = Path.GetDirectoryName(Path.GetFullPath(projectPath)) ?? "";
            var ownConfig = Path.Combine(ownDir, "pipeline.config.json");
            var own = File.Exists(ownConfig)
                ? ConfigStore.Load(new RealFileSystem(), ownConfig) : null;
            if (own is not null)
            {
                if (!string.IsNullOrWhiteSpace(own.Inbox)) Config.Inbox = own.Inbox;
                if (!string.IsNullOrWhiteSpace(own.FbxOutput)) Config.FbxOutput = own.FbxOutput;
                if (!string.IsNullOrWhiteSpace(own.ImportRoot)) Config.ImportRoot = own.ImportRoot;
                if (!string.IsNullOrWhiteSpace(own.IdentityImportRoot))
                    Config.IdentityImportRoot = own.IdentityImportRoot;
                Config.IdentityAssetDir = own.IdentityAssetDir ?? "";
                Config.BindingsFile = own.BindingsFile ?? "";
                LastAction = "已切换工程：" + Path.GetFileName(projectPath)
                             + "（已载入该工程自带路径配置）";
            }
            ConfigStore.Save(ConfigPath, Config);
            Paths = new Paths(projectPath);
            BindingsPath = (Config.BindingsFile is { Length: > 0 })
                ? Config.BindingsFile
                : Path.Combine(Paths.ConfigDir, "stream_bindings.json");
            LastAction = "已切换工程：" + Path.GetFileName(projectPath);
            LastError = null;
            return true;
        }
        catch (Exception ex) { LastError = ex.Message; return false; }
    }

    public bool ConfirmBinding(string group, string identityPath)
    {
        try
        {
            if (string.IsNullOrEmpty(identityPath)) { LastError = "未选择身份资产"; return false; }
            // 绑定是真实可写文件：读写必须同一文件系统（曾传入注入的 FakeFS →
            // 写走真实磁盘、读走内存 → "清除"读到空表误判无变化，文件未更新）
            var changed = new BindingStore(new RealFileSystem(), BindingsPath).Set(group, identityPath);
            LastAction = changed
                ? $"已绑定 {group} → {Path.GetFileName(identityPath)}"
                : "绑定未变化（已是该身份）";
            return true;
        }
        catch (Exception ex) { LastError = ex.Message; return false; }
    }

    public bool ClearBinding(string group)
    {
        try
        {
            var changed = new BindingStore(new RealFileSystem(), BindingsPath).Set(group, "");
            LastAction = changed ? $"已清除 {group} 的绑定" : "该组本无绑定";
            return true;
        }
        catch (Exception ex) { LastError = ex.Message; return false; }
    }

    public void SaveConfig(PipelineConfig cfg)
    {
        try
        {
            ConfigStore.Save(ConfigPath, cfg);
            Config = cfg;
            LastAction = "配置已保存";
        }
        catch (Exception ex) { LastError = ex.Message; }
    }

    /// <summary>强制结束：intent + 杀编辑器。返回被杀进程数。</summary>
    public int ForceStop()
    {
        try { return _engine.ForceStop(Config?.Project ?? "", Paths); }
        catch (Exception ex) { LastError = ex.Message; return -1; }
    }

    private T? SafeRead<T>(Func<T?> fn, string label) where T : class
    {
        try { return fn(); }
        catch (Exception ex) { LastError = $"{label}读取失败：{ex.Message}"; return null; }
    }

    private CommandResult? Safe(Func<CommandResult> fn)
    {
        try { return fn(); }
        catch (Exception ex) { LastError = ex.Message; return null; }
    }
}
