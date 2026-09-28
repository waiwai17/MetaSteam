// 界面外壳：绑定 + 事件转发（判断全在 ViewModel，已单测）。
// 刷新在后台 Task 读取，Dispatcher 回 UI 线程 —— UE 解算阻塞 34 分钟期间照常刷新。

using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;
using MhsPipeline.Core;

namespace MhsPipeline.App;

public partial class MainWindow : Window
{
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private readonly CancellationTokenSource _cts = new();
    private readonly EditorProbe _probe = new(new WmiProcessQuery());
    private MainViewModel? _vm;
    private string _configPath = "";
    private string _launcherLog = "";
    // 绑定目标下拉：定时跟随热文件夹分组（新建 pm02/ 这类文件夹要能自动出现）
    private DateTime _lastGroupScan = DateTime.MinValue;
    private string _groupSig = "";
    private bool _suppressGroupSel;
    private string _selectedGroup = "";
    private DateTime _identityIndexMtime = DateTime.MinValue;   // 身份索引文件 mtime（变更即重读下拉）

    public MainWindow()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Closed += (_, _) => _cts.Cancel();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        _configPath = ResolveConfig() ?? "";
        if (string.IsNullOrEmpty(_configPath))
        {
            ErrorText.Text = "未找到 pipeline.config.json";
            return;
        }

        var fs = new RealFileSystem();
        var cfg = ConfigStore.Load(fs, _configPath) ?? new PipelineConfig();
        var paths = new Paths(cfg.Project ?? "");
        var reader = new StateReader(fs);
        var toolsLauncher = ResolveToolsLauncher();
        if (!File.Exists(toolsLauncher))
        {
            ErrorText.Text = "未找到 tools\\launcher.py（GUI 需与 deploy\\tools 同在一个分发目录内）";
        }
        var launcher = new LauncherClient(new RealProcessRunner(), toolsLauncher);
        _launcherLog = launcher.LogPath is { Length: > 0 }
            ? launcher.LogPath : Path.ChangeExtension(toolsLauncher, ".gui.log");
        var engine = new EngineControl(new WmiProcessQuery());
        _vm = new MainViewModel(reader, launcher, paths, fs, engine)
        {
            ConfigPath = _configPath,
            Config = cfg,
            BindingsPath = cfg.BindingsFile is { Length: > 0 }
                ? cfg.BindingsFile
                : Path.Combine(paths.ConfigDir, "stream_bindings.json"),
        };

        // 配置区初值
        ProjectText.Text = cfg.Project ?? "-";
        // 注：路径输入框统一由 RefreshConfigFields 赋值（切换工程后也会同步）
        ProjectStore.Add(ProjectStore.DefaultPath(_configPath), cfg.Project ?? "");
        RefreshProjectOptions();
        InboxBox.Text = cfg.Inbox ?? "";
        IdRootBox.Text = cfg.IdentityImportRoot ?? "";
        ImportRootBox.Text = cfg.ImportRoot ?? "";
        OutBox.Text = cfg.FbxOutput ?? "";
        InstanceBox.Text = cfg.Instance ?? "";
        WebPortBox.Text = cfg.WebPort.ToString();

        RefreshIdentityOptions();
        RefreshGroupOptions();
        _timer.Tick += (_, _) => _ = TickAsync();
        _timer.Start();
        _ = TickAsync();
    }

    /// <summary>定位 tools/launcher.py：从 exe 目录逐级向上找（Debug/publish 布局深度不同）。
    /// 曾写死 "../../../tools/launcher.py" —— Debug 布局下指向不存在的路径，
    /// 导致 GUI 的 [启动]/[预检] 实际调不到 launcher（预检误报"发现问题"）。</summary>
    private static string ResolveToolsLauncher()
    {
        for (var d = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory); d is not null; d = d.Parent)
        {
            var p = Path.Combine(d.FullName, "tools", "launcher.py");
            if (File.Exists(p)) return p;
        }
        return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "tools", "launcher.py");
    }

    private static string? ResolveConfig()
    {
        foreach (var p in Environment.GetCommandLineArgs().Skip(1))
            if (p.EndsWith(".json", StringComparison.OrdinalIgnoreCase) && File.Exists(p)) return p;
        for (var d = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory);
             d is not null; d = d.Parent)
        {
            var hit = Path.Combine(d.FullName, "pipeline.config.json");
            if (File.Exists(hit)) return hit;
        }
        return null;
    }

    private async Task TickAsync()
    {
        if (_vm is null) return;
        try
        {
            var alive = await Task.Run(() => _probe.IsRunning(_vm.Config?.Project ?? ""), _cts.Token);
            var changed = await Task.Run(() => _vm!.Refresh(alive, DateTime.UtcNow), _cts.Token);
            MaybeRefreshGroupOptions();          // 热文件夹新增分组 → 绑定目标下拉自动跟随
            if (!changed) return;
            await Dispatcher.InvokeAsync(ApplyToUi);
        }
        catch (Exception ex)
        {
            ErrorText.Text = ex.Message;
        }
    }

    /// <summary>每 5 秒检查热文件夹子目录变化（新建分组如 pm02/）→ 自动刷新绑定目标候选。
    /// 曾只在"改路径/保存/切工程"时刷新 → 录制中新建分组，下拉看不到它。</summary>
    private void MaybeRefreshGroupOptions()
    {
        if (_vm is null) return;
        if ((DateTime.UtcNow - _lastGroupScan).TotalSeconds < 5) return;
        _lastGroupScan = DateTime.UtcNow;
        string sig;
        try
        {
            var inbox = InboxBox.Text.Trim();
            sig = string.Join(",", GroupOptions.List(new RealFileSystem(), inbox)
                                             .Select(o => o.Value));
        }
        catch { return; }
        if (sig == _groupSig) return;
        var isFirst = string.IsNullOrEmpty(_groupSig);
        _groupSig = sig;
        RefreshGroupOptions(preserveSelection: true);
        if (!isFirst) ActionText.Text = "热文件夹分组已更新（绑定目标候选自动刷新）";
    }

    private void ApplyToUi()
    {
        var vm = _vm!;
        Title = $"mhs_pipeline · 流式解算监控 — {vm.TitleInfo}";

        // 状态行：分段着色（状态词带色/计数灰/ETA 蓝）
        StateText.Text = vm.StateText;
        StateText.Foreground = StateBrushFor(vm.EngineState);
        CountsLine.Text = vm.CountsLine;
        EtaText.Text = vm.EtaText;
        EtaText.Visibility = string.IsNullOrEmpty(vm.EtaText) ? Visibility.Collapsed : Visibility.Visible;
        DataAge.Text = vm.DataAgeText;
        IdentityBandPanel.Visibility = vm.HasIdentity ? Visibility.Visible : Visibility.Collapsed;
        // 索引变化（引擎启动即生成 / 空闲期每 30s 自动重写 / [刷新] 触发）→ 下拉随之同步。
        // 判据用**文件 mtime**（比"数量变化"稳）：只要索引重写过就重读一次。
        try
        {
            var idx = vm.Paths.IdentityIndex;
            var mt = File.Exists(idx) ? File.GetLastWriteTimeUtc(idx) : DateTime.MinValue;
            if (mt != _identityIndexMtime)
            {
                _identityIndexMtime = mt;
                RefreshIdentityOptions();
            }
            else if (IdentityCombo.ItemsSource is not System.Collections.IList cur
                     || cur.Count != vm.IdentityOptions.Count)
            {
                RefreshIdentityOptions();
            }
        }
        catch { /* 索引文件不可读 → 保持现状 */ }
        IdGroup.Text = vm.IdentityGroupText;
        IdPath.Text = vm.IdentityPathText;
        IdTail.Text = vm.IdentityTailText;
        ProgressTitle.Text = vm.ProgressTitle;
        ProgressDetail.Text = vm.ProgressDetail;
        ProgressBar.Value = vm.ProgressPercent;
        ProgressCard.Visibility = vm.HasProgress ? Visibility.Visible : Visibility.Collapsed;
        EngineStateText.Text = vm.EngineStateText;
        StateDot.Fill = BrushFor(vm.EngineState);
        LiveDot.Fill = BrushFor(vm.EngineState);
        ActionText.Text = vm.LastAction ?? "";
        ErrorText.Text = vm.LastError ?? "";

        // 模块常显（原型布局）：计数右对齐 "N 条"，空态给一行 dim 提示
        LoadingCount.Text = $"{vm.LoadingCount} 条";
        ActiveCount.Text = $"{vm.ActiveCount} 条";
        FailedCount.Text = $"{vm.FailedCount} 条";
        DoneCount.Text = $"{vm.DoneCount} 条 · 仅表演交付";
        LoadingEmpty.Visibility = vm.LoadingCount == 0 ? Visibility.Visible : Visibility.Collapsed;
        ActiveEmpty.Visibility = vm.ActiveCount == 0 ? Visibility.Visible : Visibility.Collapsed;
        FailedEmpty.Visibility = vm.FailedCount == 0 ? Visibility.Visible : Visibility.Collapsed;
        DoneEmpty.Visibility = vm.DoneCount == 0 ? Visibility.Visible : Visibility.Collapsed;

        LoadingList.ItemsSource = WrapLoading(vm.LoadingLines);
        ActiveList.ItemsSource = Wrap(vm.ActiveLines, BlueB);
        FailedList.ItemsSource = Wrap(vm.FailedLines, RedB);
        DoneList.ItemsSource = Wrap(vm.DoneLines, GreenB);
        EventList.ItemsSource = WrapEvents(vm.EventLines);
        GroupMatrix.ItemsSource = WrapGroups(vm.GroupMatrixLines);
    }

    private static Brush BrushFor(string state) => StateBrushFor(state);

    // ── 列表行按状态着色（V6 原型：完成绿/失败红/运行蓝/待处理琥珀/未绑定琥珀）──
    private sealed record RowVm(string Text, Brush Fg);

    private Brush GreenB => (TryFindResource("Green") as Brush) ?? Brushes.LimeGreen;
    private Brush RedB => (TryFindResource("Red") as Brush) ?? Brushes.OrangeRed;
    private Brush BlueB => (TryFindResource("Blue") as Brush) ?? Brushes.DodgerBlue;
    private Brush AmberB => (TryFindResource("Amber") as Brush) ?? Brushes.Orange;
    private Brush WhtB => (TryFindResource("Wht") as Brush) ?? Brushes.White;

    private static RowVm[] Wrap(IEnumerable<string>? lines, Brush fg)
        => lines?.Select(l => new RowVm(l, fg)).ToArray() ?? Array.Empty<RowVm>();

    private RowVm[] WrapEvents(IEnumerable<string>? lines)
        => lines?.Select(l => new RowVm(l, EventBrush(l))).ToArray() ?? Array.Empty<RowVm>();

    private RowVm[] WrapGroups(IEnumerable<string>? lines)
        => lines?.Select(l => new RowVm(l, l.Contains("<未绑定>") ? AmberB : GreenB)).ToArray()
           ?? Array.Empty<RowVm>();

    private RowVm[] WrapLoading(IEnumerable<string>? lines)
    {
        // 加载中行：[盘]（未接管）琥珀 / 其余（等待就绪/待绑定）灰白区分度低 → 统一琥珀偏弱
        return lines?.Select(l => new RowVm(l, l.Contains("[盘]") ? AmberB : WhtB)).ToArray()
               ?? Array.Empty<RowVm>();
    }

    /// <summary>事件行级别着色："HH:mm 级别 …"（完成/失败/运行/待处理）。</summary>
    private Brush EventBrush(string line)
    {
        var sp = line.IndexOf(' ');
        var rest = sp >= 0 && sp + 1 < line.Length ? line[(sp + 1)..] : line;
        var lv = rest.Split(' ')[0];
        return lv switch
        {
            "完成" => GreenB,
            "失败" => RedB,
            "运行" => BlueB,
            "待处理" => AmberB,
            _ => WhtB
        };
    }

    /// <summary>状态词颜色：绿=监听中 琥珀=暂停中 橙红=无响应 红=已中断 灰=停止。</summary>
    private static Brush StateBrushFor(string state) => state switch
    {
        "healthy" or "idle" => Brushes.LimeGreen,
        "paused" => Brushes.Orange,
        "stalled" => Brushes.OrangeRed,
        "dead" => Brushes.Red,
        "stopped" => Brushes.DimGray,
        _ => Brushes.DimGray
    };

    // ── 事件 ──
    /// <summary>[启动 / 继续]：发出命令后轮询 watchdog 状态文件，给出真实反馈
    /// （曾"点了没反应"——两层原因：管道 bug 杀死了 launcher，且结果从未展示）。</summary>
    private async void OnStart(object sender, RoutedEventArgs e)
        => await StartAndWaitAsync(sender as System.Windows.Controls.Button, "引擎加载约 30~90 秒", 75);

    /// <summary>启动并等 watchdog 就绪（供 [启动] 与"新建后预热"共用）。</summary>
    private async Task StartAndWaitAsync(System.Windows.Controls.Button? btn, string hint, int waitSeconds)
    {
        if (_vm is null) return;
        if (btn is not null) btn.IsEnabled = false;
        ActionText.Text = "启动中…（" + hint + "）";
        ErrorText.Text = "";
        try
        {
            await Task.Run(() => _vm.Start());
            var deadline = DateTime.UtcNow.AddSeconds(waitSeconds);
            while (DateTime.UtcNow < deadline)
            {
                await Task.Delay(2000);
                switch (TryReadEngineState())
                {
                    case "healthy":
                    case "idle":
                        ActionText.Text = "已启动 · 监听中";
                        return;
                    case "already_running":
                        ErrorText.Text = "已有编辑器在跑本工程（launcher 拒绝再起一个）";
                        ActionText.Text = "启动被拒";
                        return;
                    case "precheck_failed":
                        ErrorText.Text = "预检未通过（先点 [预检] 看明细）";
                        ActionText.Text = "启动被拒";
                        return;
                }
            }
            ErrorText.Text = $"{waitSeconds} 秒内未见 watchdog 状态，可能仍在加载或已失败。日志：" + _launcherLog;
        }
        finally { if (btn is not null) btn.IsEnabled = true; }
    }

    private string? TryReadEngineState()
    {
        try
        {
            if (_vm is null || !File.Exists(_vm.Paths.EngineStatus)) return null;
            return Jsonx.TryParse<EngineStatus>(File.ReadAllText(_vm.Paths.EngineStatus))?.State;
        }
        catch { return null; }
    }

    /// <summary>[打开工程]：以带界面方式打开当前工程（制作 / 检查身份用）。
    /// 无头管线正在跑时先警告——同一工程的两个编辑器会共写同一份队列（实测会互相覆盖）。</summary>
    private void OnOpenProjectEditor(object sender, RoutedEventArgs e)
    {
        if (_vm?.Config is null) return;
        var project = _vm.Config.Project ?? "";
        var editor = _vm.Config.UeEditor ?? "";
        if (string.IsNullOrWhiteSpace(project) || !File.Exists(project))
        {
            ErrorText.Text = "工程文件不存在：" + project;
            return;
        }
        if (string.IsNullOrWhiteSpace(editor) || !File.Exists(editor))
        {
            ErrorText.Text = "编辑器不存在（配置项 ue_editor）：" + editor;
            return;
        }
        try
        {
            var alive = _probe.IsRunning(project);
            if (alive && MessageBox.Show(
                    "无头管线正在跑这个工程。\n同时用编辑器打开会共写同一份队列（交付可能错乱）。\n\n建议：先点 [暂停] 等它退出，再打开工程。\n\n仍要继续打开吗？",
                    "打开工程（制作身份）", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
            {
                ActionText.Text = "已取消（先 [暂停] 再打开工程更安全）";
                return;
            }
            Process.Start(new ProcessStartInfo(editor) { ArgumentList = { project } });
            ActionText.Text = "已打开工程（制作完身份请保存并关闭编辑器，再点 [启动 / 继续]）";
            ErrorText.Text = "";
        }
        catch (Exception ex)
        {
            ErrorText.Text = "打开工程失败：" + ex.Message;
        }
    }

    private async void OnPause(object sender, RoutedEventArgs e)
    {
        if (_vm is null) return;
        ActionText.Text = "正在请求暂停（跑完当前条后退出）…";
        var res = await Task.Run(() => _vm.Pause());
        if (res is null) { ErrorText.Text = _vm.LastError ?? "暂停请求失败"; return; }
        ActionText.Text = "暂停请求已发出（当前条完成后退出，队列保留）";
    }

    private void OnForceStop(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show("立即结束引擎进程？（队列保留，之后可启动/继续）",
                "强制结束", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
            return;
        var n = _vm?.ForceStop() ?? -1;
        ActionText.Text = n > 0 ? $"已结束 {n} 个引擎进程" : "没有匹配的编辑器进程";
    }

    private void OnConfirmBinding(object sender, RoutedEventArgs e)
    {
        var identity = IdentityCombo.SelectedValue as string;
        var group = GroupCombo.SelectedValue as string ?? "";
        if (_vm is null) return;
        if (string.IsNullOrEmpty(identity))
        {
            ErrorText.Text = "先选择身份资产";
            return;
        }
        if (_vm.ConfirmBinding(group, identity))
        {
            ActionText.Text = _vm.LastAction;
            ErrorText.Text = "";
        }
        else ErrorText.Text = _vm.LastError ?? "绑定失败";
    }

    private void OnClearBinding(object sender, RoutedEventArgs e)
    {
        var group = GroupCombo.SelectedValue as string ?? "";
        if (_vm is null) return;
        if (_vm.ClearBinding(group)) ActionText.Text = _vm.LastAction;
        else ErrorText.Text = _vm.LastError ?? "清除失败";
    }

    private void OnSaveConfig(object sender, RoutedEventArgs e)
    {
        if (_vm?.Config is null) return;
        var cfg = _vm.Config;
        cfg.Inbox = InboxBox.Text.Trim();
        cfg.IdentityImportRoot = IdRootBox.Text.Trim();
        cfg.ImportRoot = ImportRootBox.Text.Trim();
        cfg.FbxOutput = OutBox.Text.Trim();
        cfg.Instance = InstanceBox.Text.Trim();
        if (int.TryParse(WebPortBox.Text.Trim(), out var port)) cfg.WebPort = port;
        _vm.SaveConfig(cfg);
        ActionText.Text = _vm.LastAction ?? "";
        ErrorText.Text = _vm.LastError ?? "";
        RefreshGroupOptions();      // inbox 可能改了 → 绑定目标候选刷新
    }

    /// <summary>[刷新]：请求引擎重写身份索引 → 等它写完 → **重新读取并刷新下拉**。
    /// 曾只写 refresh_assets.flag 就结束（下拉从不更新 → 点了"没反应"）；
    /// 且引擎未运行时会被静默忽略 —— 现在如实提示。
    /// </summary>
    private async void OnRefreshIndex(object sender, RoutedEventArgs e)
    {
        if (_vm is null) return;
        var btn = sender as System.Windows.Controls.Button;
        if (btn is not null) btn.IsEnabled = false;
        try
        {
            var alive = await Task.Run(() => _probe.IsRunning(_vm.Config?.Project ?? ""));
            if (!alive)
            {
                // 索引只能由引擎进程内生成（AssetRegistry）→ 引擎没跑时刷新必然无效。
                // 与其只提示，不如直接提供"顺手启动"：启动后索引会立刻生成、下拉自动跟上。
                var go = MessageBox.Show(
                    "身份索引由引擎进程内生成，现在没有引擎在运行，所以刷新不会生效。\n\n" +
                    "是否现在启动引擎？（约 30~60 秒，启动后索引会自动生成，下拉会自己出现新身份，无需再点刷新）",
                    "刷新身份资产", MessageBoxButton.OKCancel, MessageBoxImage.Information);
                ActionText.Text = "刷新未执行（引擎未运行）";
                if (go == MessageBoxResult.OK)
                {
                    await StartAndWaitAsync(null, "引擎加载约 30~90 秒", 75);
                    await Task.Delay(3000);
                    RefreshIdentityOptions();
                    ActionText.Text = _vm.IdentityOptions.Count > 0
                        ? $"身份资产已刷新：{_vm.IdentityOptions.Count} 项" : "引擎已启动（索引稍后生成）";
                }
                return;
            }
            File.WriteAllText(Path.Combine(_vm.Paths.ConfigDir, "refresh_assets.flag"),
                              DateTime.Now.ToString("s"));
            ActionText.Text = "已请求刷新身份索引…";
            await Task.Delay(4000);                 // 驱动下一轮（<5s）重写 identity_assets.json
            RefreshIdentityOptions();               // 重新读入并刷新下拉
            var n = _vm.IdentityOptions.Count;
            ActionText.Text = n > 0
                ? "身份资产已刷新：" + n + " 项（含大小，选错前就看得见）"
                : "未找到身份资产（先在 UE 里制作并保存身份，再刷新）";
            ErrorText.Text = "";
        }
        catch (Exception ex) { ErrorText.Text = ex.Message; }
        finally { if (btn is not null) btn.IsEnabled = true; }
    }

    private void RefreshIdentityOptions()
    {
        _vm?.ReloadIdentityOptions();
        IdentityCombo.ItemsSource = _vm?.IdentityOptions;
        if (_vm?.IdentityOptions.Count > 0) IdentityCombo.SelectedIndex = 0;
    }

    /// <summary>刷新绑定目标候选（= 加载路径子文件夹）。preserveSelection=true 时保留当前选择
    /// （定时刷新不能把用户刚选的组给顶掉）。</summary>
    private void RefreshGroupOptions(bool preserveSelection = false)
    {
        var opts = GroupOptions.List(new RealFileSystem(), InboxBox.Text.Trim());
        var keep = _selectedGroup;
        GroupCombo.ItemsSource = opts;
        var idx = preserveSelection && !string.IsNullOrEmpty(keep)
            ? opts.FindIndex(o => o.Value == keep)                       // 保留用户当前选择
            : -1;
        if (idx < 0) idx = Math.Max(0, opts.FindIndex(o => o.Display != "(根 · 全部)"));
        _suppressGroupSel = true;                                        // 程序化选中 ≠ 用户手选
        try { GroupCombo.SelectedIndex = idx; }
        finally { _suppressGroupSel = false; }
        if (idx >= 0) _selectedGroup = opts[idx].Value;
    }

    private void OnGroupSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressGroupSel) return;                    // 定时刷新引起的选中不算用户选择
        if (GroupCombo.SelectedValue is string v) _selectedGroup = v;
    }

    private void OnBrowseInbox(object sender, RoutedEventArgs e) => BrowseFor(InboxBox, false);
    private void OnBrowseOut(object sender, RoutedEventArgs e) => BrowseFor(OutBox, false);
    private void OnBrowseIdRoot(object sender, RoutedEventArgs e) => BrowseFor(IdRootBox, true);
    private void OnBrowseImportRoot(object sender, RoutedEventArgs e) => BrowseFor(ImportRootBox, true);

    /// <summary>选择文件夹。contentMode=true → UE 内容路径（/Game/…，根为 <工程>/Content）。
    /// 用自绘对话框：Windows 外壳对话框实测会阻塞 UI 线程（窗口变灰"未响应"）。</summary>
    private void BrowseFor(System.Windows.Controls.TextBox box, bool contentMode)
    {
        if (_vm is null) return;
        var dlg = new FolderPickerWindow(contentMode, _vm.Config?.Project ?? "", box.Text.Trim())
        { Owner = this };
        if (dlg.ShowDialog() == true && !string.IsNullOrEmpty(dlg.ResultPath))
        {
            box.Text = dlg.ResultPath;
            ActionText.Text = (contentMode ? "已选内容路径：" : "已选文件夹：") + dlg.ResultPath;
            // 加载路径变了 → 绑定目标候选 = 该路径子文件夹，必须即时刷新
            // （否则要等 [保存为默认] 才更新，用户会以为候选是写死的）
            if (ReferenceEquals(box, InboxBox)) RefreshGroupOptions();
        }
    }

    /// <summary>加载路径输入框失焦/回车 → 刷新绑定目标候选（候选 = 子文件夹，动态）。</summary>
    private void OnInboxEdited(object sender, RoutedEventArgs e) => RefreshGroupOptions();

    private void OnInboxKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) { RefreshGroupOptions(); e.Handled = true; }
    }

    /// <summary>预检：调 launcher doctor（只读，不起引擎）→ 结果**内联**显示在引擎控制卡下方。
    /// （不用弹窗：GUI 弹窗在自动化/受限环境下不可靠；内联结果一眼可见、可滚动复制）</summary>
    private async void OnPrecheck(object sender, RoutedEventArgs e)
    {
        if (_vm is null) return;
        PrecheckBtn.IsEnabled = false;
        ActionText.Text = "预检中…";
        try
        {
            var res = await Task.Run(() => _vm.Precheck());
            if (res is null)
            {
                ErrorText.Text = "预检失败：" + (_vm.LastError ?? "未知错误（检查 tools\\launcher.py 与 python）");
                PrecheckHead.Text = "预检失败";
                PrecheckHead.Foreground = RedB;
                PrecheckBody.Text = _vm.LastError ?? "";
                PrecheckBox.Visibility = Visibility.Visible;
                return;
            }
            var ok = res.ExitCode == 0;
            PrecheckHead.Text = ok ? "预检通过（可以启动）" : $"预检发现问题（退出码 {res.ExitCode}）";
            PrecheckHead.Foreground = ok ? GreenB : RedB;
            PrecheckBody.Text = res.Output ?? "";
            PrecheckBox.Visibility = Visibility.Visible;
            ActionText.Text = ok ? "预检通过" : "预检发现问题（见左侧结果）";
            ErrorText.Text = ok ? "" : "预检未通过：先按左侧结果修正";
        }
        catch (Exception ex)
        {
            ErrorText.Text = "预检失败：" + ex.Message;
        }
        finally
        {
            PrecheckBtn.IsEnabled = true;
        }
    }

    private void OnOpenOutput(object sender, RoutedEventArgs e)
    {
        var dir = OutBox.Text.Trim();
        if (Directory.Exists(dir)) Process.Start("explorer.exe", dir);
        else ErrorText.Text = "输出目录不存在";
    }

    // ── 工程选择 ──
    private sealed record ProjectItem(string Path, string Display);

    /// <summary>把配置值刷进路径输入框（启动 + 切换工程后调用，避免界面显示旧工程的路径）。</summary>
    private void RefreshConfigFields()
    {
        var c = _vm?.Config;
        if (c is null) return;
        ProjectText.Text = c.Project ?? "";
        InboxBox.Text = c.Inbox ?? "";
        IdRootBox.Text = c.IdentityImportRoot ?? "";
        ImportRootBox.Text = c.ImportRoot ?? "";
        OutBox.Text = c.FbxOutput ?? "";
        InstanceBox.Text = c.Instance ?? "";
        WebPortBox.Text = c.WebPort.ToString();
    }

    private void RefreshProjectOptions()
    {
        if (_vm is null) return;
        var cur = _vm.Config?.Project ?? "";
        var list = new List<ProjectItem>();

        void Push(string p)
        {
            if (string.IsNullOrWhiteSpace(p) || !File.Exists(p)) return;
            // 归一化：配置里是正斜杠、历史里是反斜杠 → 不归一会把同一工程列两遍
            var full = Path.GetFullPath(p);
            if (list.Any(i => i.Path.Equals(full, StringComparison.OrdinalIgnoreCase))) return;
            list.Add(new ProjectItem(full, ProjectLabel(full)));
        }

        Push(cur);
        foreach (var p in ProjectStore.Load(ProjectStore.DefaultPath(_configPath))) Push(p);
        // 内置模板（deploy/template/MH_Template）
        var cfgDir = Path.GetDirectoryName(Path.GetFullPath(_configPath)) ?? "";
        Push(Path.GetFullPath(Path.Combine(cfgDir, "..", "template", "MH_Template", "MH_Template.uproject")));

        ProjectCombo.ItemsSource = list;
        var hit = list.FindIndex(i => i.Path.Equals(cur, StringComparison.OrdinalIgnoreCase));
        ProjectCombo.SelectedIndex = Math.Max(0, hit);
    }

    private static string ProjectLabel(string p)
    {
        var dir = Path.GetDirectoryName(p) ?? "";
        var parent = Path.GetFileName(dir);
        var grand = Path.GetFileName(Path.GetDirectoryName(dir) ?? "");
        var loc = string.IsNullOrEmpty(grand) ? parent : grand + "/" + parent;
        return $"{Path.GetFileNameWithoutExtension(p)}  ·  {loc}";
    }

    private void OnProjectPicked(object sender, SelectionChangedEventArgs e)
    {
        if (_vm is null || ProjectCombo.SelectedValue is not string path) return;
        if (string.IsNullOrEmpty(path)) return;
        var prev = _vm.Config?.Project ?? "";
        if (path.Equals(prev, StringComparison.OrdinalIgnoreCase)) return;
        SwitchTo(path, prev);
    }

    private void OnBrowseProject(object sender, RoutedEventArgs e)
    {
        if (_vm is null) return;
        var prev = _vm.Config?.Project ?? "";
        var dlg = new FolderPickerWindow(false, prev, Path.GetDirectoryName(prev) ?? "", "*.uproject")
        { Owner = this };
        if (dlg.ShowDialog() != true || string.IsNullOrEmpty(dlg.ResultPath)) return;
        SwitchTo(dlg.ResultPath, prev);
    }

    private void OnScanProjects(object sender, RoutedEventArgs e)
    {
        if (_vm is null) return;
        var dlg = new FolderPickerWindow(false, _vm.Config?.Project ?? "",
            Path.GetDirectoryName(_vm.Config?.Project ?? "") ?? "") { Owner = this };
        if (dlg.ShowDialog() != true || string.IsNullOrEmpty(dlg.ResultPath)) return;
        var found = ProjectDiscovery.Scan(dlg.ResultPath, 3);
        var store = ProjectStore.DefaultPath(_configPath);
        foreach (var p in found) ProjectStore.Add(store, p);
        RefreshProjectOptions();
        ActionText.Text = found.Count == 0
            ? $"未在 {dlg.ResultPath} 下发现 .uproject"
            : $"扫描到 {found.Count} 个工程（已加入下拉）";
    }

    /// <summary>切换工程：落配置 + 重建路径 + 刷新依赖路径的控件。</summary>
    private void SwitchTo(string path, string prev)
    {
        if (_vm is null) return;
        if (!_vm.SwitchProject(path))
        {
            ErrorText.Text = _vm.LastError ?? "切换失败";
            RefreshProjectOptions();
            return;
        }
        ProjectStore.Add(ProjectStore.DefaultPath(_configPath), path);
        RefreshConfigFields();      // 路径四项可能随工程自带配置变化 → 界面同步
        ActionText.Text = _vm.LastAction ?? "";
        ErrorText.Text = "";
        // 原工程的编辑器可能仍在跑：GUI 只读监控当前所选工程 → 如实提示，不擅自干预
        if (!string.IsNullOrEmpty(prev) && _probe.IsRunning(prev))
            ErrorText.Text = "注意：原工程编辑器仍在运行（GUI 只监控当前所选工程）";
        RefreshIdentityOptions();
        RefreshGroupOptions();
        RefreshProjectOptions();
        _ = TickAsync();
    }

    private void OnOpenProjectDir(object sender, RoutedEventArgs e)
    {
        var dir = Path.GetDirectoryName(_vm!.Paths.ConfigDir);
        if (dir is not null && Directory.Exists(dir)) Process.Start("explorer.exe", dir);
    }

    private async void OnNewProject(object sender, RoutedEventArgs e)
    {
        var dlg = new NewProjectDialog { Owner = this };
        if (dlg.ShowDialog() == true)
        {
            var res = _vm?.NewProject(dlg.ProjectName, dlg.TargetDir);
            if (res is not null && res.ExitCode != 0) { ErrorText.Text = res.Output; return; }
            // 新建后直接切过去（模板已含插件与默认配置 → 其 pipeline.config.json 随工程走）
            var prev = _vm?.Config?.Project ?? "";
            var uproj = Path.Combine(dlg.TargetDir, dlg.ProjectName, dlg.ProjectName + ".uproject");
            if (_vm is not null && File.Exists(uproj))
            {
                SwitchTo(uproj, prev);
                // 首次启动要为新工程装 Python 依赖（实测 ~2~3 分钟）——此刻预热，
                // 录制开跑时就不会出现"[盘] 已到达却没人接管"的空窗
                if (MessageBox.Show(
                        "工程已创建并切换。\n\n是否现在**预热启动一次**？新工程首次启动需要初始化 Python 依赖（约 2~3 分钟，一次即可；之后启动只需 30~60 秒）。\n\n建议：现在预热，录制时立即可用。",
                        "新建工程 · 预热", MessageBoxButton.OKCancel, MessageBoxImage.Question) == MessageBoxResult.OK)
                {
                    await StartAndWaitAsync(null, "新工程首次启动约 2~3 分钟（初始化 Python 环境）", 300);
                }
                return;
            }
            ActionText.Text = $"工程已创建：{dlg.ProjectName}（未找到 {uproj}，请用 [浏览…] 选择）";
        }
    }

    // ── 事件筛选 ──
    private void ApplyFilter(EventFilter f)
    {
        if (_vm is null) return;
        _vm.Filter = f;
        EventList.ItemsSource = WrapEvents(_vm.EventLines);
    }

    private void OnFilterAll(object sender, RoutedEventArgs e) => ApplyFilter(EventFilter.All);
    private void OnFilterFailed(object sender, RoutedEventArgs e) => ApplyFilter(EventFilter.Failed);
    private void OnFilterPending(object sender, RoutedEventArgs e) => ApplyFilter(EventFilter.Pending);
}
