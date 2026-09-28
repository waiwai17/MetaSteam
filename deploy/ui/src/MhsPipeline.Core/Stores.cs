// 可写存储：pipeline.config.json（配置）与 stream_bindings.json（绑定）。
// 纪律：GUI 只写"配置与绑定"两类文件，绝不写队列（队列唯一事实源在引擎侧）。
// 绑定写入后由引擎侧 _check_bindings_touched 热应用（mtime 感知，已测）。

using System.Text.Json;
using System.Text.Json.Serialization;

namespace MhsPipeline.Core;

public sealed class PipelineConfig
{
    [JsonPropertyName("instance")] public string Instance { get; set; } = "default";
    [JsonPropertyName("ue_editor")] public string UeEditor { get; set; } = "";
    [JsonPropertyName("project")] public string Project { get; set; } = "";
    [JsonPropertyName("inbox")] public string Inbox { get; set; } = "";
    [JsonPropertyName("fbx_output")] public string FbxOutput { get; set; } = "";
    [JsonPropertyName("identity_import_root")] public string IdentityImportRoot { get; set; } = "/Game/CaptureManager/ID";
    /// <summary>身份资产目录：**做好的** MetaHumanIdentity 存放处（供 [检测]/扫描用）。
    /// 与 identity_import_root 语义不同：后者是录制的 ID 素材（ROM）导入位置（制作原料）。
    /// 留空时回落 identity_import_root（兼容旧配置）。</summary>
    [JsonPropertyName("identity_asset_dir")] public string IdentityAssetDir { get; set; } = "";
    [JsonPropertyName("import_root")] public string ImportRoot { get; set; } = "/Game/CaptureManager/Auto";
    [JsonPropertyName("bindings_file")] public string BindingsFile { get; set; } = "";
    [JsonPropertyName("web_port")] public int WebPort { get; set; } = 8902;
    [JsonPropertyName("bootstrap")] public string Bootstrap { get; set; } = "";
}

public static class ConfigStore
{
    private static readonly JsonSerializerOptions Opts = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never
    };

    public static PipelineConfig? Load(IFileSystem fs, string path)
        => Jsonx.TryParse<PipelineConfig>(fs.ReadText(path));

    /// <summary>原子保存（tmp + replace）；失败抛异常（调用方呈现给用户，不静默丢配置）。</summary>
    public static void Save(string path, PipelineConfig cfg)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(cfg, Opts));
        File.Move(tmp, path, overwrite: true);
    }
}

public sealed class BindingStore
{
    private readonly string _path;
    private readonly IFileSystem _fs;

    public BindingStore(IFileSystem fs, string path) { _fs = fs; _path = path; }

    public Dictionary<string, string> Load()
    {
        var doc = Jsonx.TryParse<BindingDoc>(_fs.ReadText(_path));
        return doc?.Bindings is null ? new() : new Dictionary<string, string>(doc.Bindings);
    }

    /// <summary>写绑定（identity 为空 = 清除该组）。返回是否变化。</summary>
    public bool Set(string group, string identity)
    {
        var all = Load();
        var changed = identity.Length > 0
            ? (!all.TryGetValue(group, out var cur) || cur != identity)
            : all.Remove(group);
        if (identity.Length > 0) all[group] = identity;
        if (!changed) return false;
        Save(all);
        return true;
    }

    private void Save(Dictionary<string, string> all)
    {
        var dir = Path.GetDirectoryName(_path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var tmp = _path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(
            new BindingDoc { Updated = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"), Bindings = all },
            new JsonSerializerOptions { WriteIndented = true }));
        File.Move(tmp, _path, overwrite: true);
    }

    private sealed class BindingDoc
    {
        [JsonPropertyName("updated")] public string Updated { get; set; } = "";
        [JsonPropertyName("bindings")] public Dictionary<string, string>? Bindings { get; set; }
    }
}

/// <summary>已知工程清单（projects.json，与 pipeline.config.json 同级）：
/// 历史用过的工程 + 内置模板 + 扫描结果，供"选择已有工程"下拉使用。</summary>
public static class ProjectStore
{
    private const int Max = 12;

    public static string DefaultPath(string configPath)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(configPath));
        return Path.Combine(string.IsNullOrEmpty(dir) ? "." : dir, "projects.json");
    }

    /// <summary>已记住且仍存在的工程（最新在前；不存在的自动剔除——不展示打不开的项）。</summary>
    public static List<string> Load(string path)
    {
        var doc = Jsonx.TryParse<ProjectDoc>(File.Exists(path) ? File.ReadAllText(path) : null);
        if (doc?.Projects is null) return new List<string>();
        return doc.Projects
            .Where(p => !string.IsNullOrWhiteSpace(p) && File.Exists(p))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    /// <summary>记住一个工程（去重、最新在前、超上限截断）。不存在的文件不记录。</summary>
    public static void Add(string path, string project)
    {
        if (string.IsNullOrWhiteSpace(project) || !File.Exists(project)) return;
        var all = Load(path);
        all.RemoveAll(p => p.Equals(project, StringComparison.OrdinalIgnoreCase));
        all.Insert(0, project);
        if (all.Count > Max) all = all.Take(Max).ToList();
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(
            new ProjectDoc { Projects = all }, new JsonSerializerOptions { WriteIndented = true }));
        File.Move(tmp, path, overwrite: true);
    }

    private sealed class ProjectDoc
    {
        [JsonPropertyName("projects")] public List<string>? Projects { get; set; }
    }
}

/// <summary>扫描目录找 .uproject（限深度 + 跳过引擎中间目录，避免全盘遍历）。</summary>
public static class ProjectDiscovery
{
    public static List<string> Scan(string root, int maxDepth = 3)
    {
        var found = new List<string>();
        if (string.IsNullOrWhiteSpace(root) || !Directory.Exists(root)) return found;
        try { Walk(root, 0, maxDepth, found); } catch { /* 无权限 → 返回已找到的 */ }
        return found.Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(p => p).ToList();
    }

    private static void Walk(string dir, int depth, int max, List<string> found)
    {
        if (depth > max) return;
        try
        {
            foreach (var f in Directory.EnumerateFiles(dir, "*.uproject")) found.Add(f);
            if (depth == max) return;
            foreach (var d in Directory.EnumerateDirectories(dir))
            {
                var name = Path.GetFileName(d);
                if (name.StartsWith(".") ||
                    name.Equals("Intermediate", StringComparison.OrdinalIgnoreCase) ||
                    name.Equals("Binaries", StringComparison.OrdinalIgnoreCase) ||
                    name.Equals("DerivedDataCache", StringComparison.OrdinalIgnoreCase) ||
                    name.Equals("Saved", StringComparison.OrdinalIgnoreCase) ||
                    name.Equals("Plugins", StringComparison.OrdinalIgnoreCase)) continue;
                Walk(d, depth + 1, max, found);
            }
        }
        catch { /* 单目录失败不影响其余 */ }
    }
}

/// <summary>绑定目标候选（正式类型，非元组）。
/// 教训：曾用 ValueTuple —— XAML 的 {Binding Display}/SelectedValuePath="Value" 对元组
/// 无法解析（元组名仅编译期存在，反射只有 Item1/Item2）→ 绑定静默失败，下拉一片空白。</summary>
public sealed class GroupOption
{
    public string Value { get; init; } = "";
    public string Display { get; init; } = "";
}

public static class GroupOptions
{
    /// <summary>绑定目标候选 = inbox 子目录（排除 _id）+ 根组。</summary>
    public static List<GroupOption> List(IFileSystem fs, string inbox)
    {
        var result = new List<GroupOption>();
        try
        {
            foreach (var d in Directory.GetDirectories(inbox).OrderBy(p => p))
            {
                var name = Path.GetFileName(d);
                if (name.Equals("_id", StringComparison.OrdinalIgnoreCase)) continue;
                result.Add(new GroupOption { Value = name, Display = name + "/" });
            }
        }
        catch { /* inbox 不存在/无权限 → 只有根组 */ }
        result.Add(new GroupOption { Value = "", Display = "(根 · 全部)" });
        return result;
    }
}

/// <summary>默认工作区根目录策略。
/// 规则：固定盘中"剩余空间最多的非系统盘"，没有则回落用户目录。
/// 为什么不全放 C:：素材与解算中间产物体积大（单条 ~200MB，全量 87 条约 17GB+），
/// 系统盘常是容量较小的 SSD，且重装系统会丢交付物。
/// 为什么不写死盘符：不同机器盘符不同（实测本机最大的是 G:）→ 运行时探测；
/// 用户仍可用 [浏览] 改到任何位置，配置只是默认值。
/// </summary>
public static class Workspace
{
    public static string DefaultRoot()
    {
        try
        {
            var sys = (Environment.GetFolderPath(Environment.SpecialFolder.Windows) ?? "C:\\")
                .Substring(0, 2).ToUpperInvariant();
            var best = "";
            var sysBest = "";
            long bestFree = -1, bestSysFallback = -1;
            foreach (var d in DriveInfo.GetDrives())
            {
                if (d.DriveType != DriveType.Fixed || !d.IsReady) continue;
                long free;
                try { free = d.AvailableFreeSpace; } catch { continue; }
                var root = d.Name.Substring(0, 2).ToUpperInvariant();
                if (root == sys) { if (free > bestSysFallback) { bestSysFallback = free; sysBest = d.Name; } continue; }
                if (free > bestFree) { bestFree = free; best = d.Name; }
            }
            if (best.Length == 0) best = sysBest.Length > 0 ? sysBest : "C:\\";
            return Path.Combine(best, "MetaSol");
        }
        catch
        {
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) ?? ".", "MetaSol");
        }
    }
}

/// <summary>盘面扫描：找出"已到达但队列未知"的 take。
/// 引擎解算阻塞期 executor 不扫盘（段间补扫才接管）——GUI 在引擎外可以自己扫盘，
/// 让用户"拖入素材"立刻有反馈（与 UE 控制台 [盘] 行同语义），而不是加载区空白。</summary>
public static class InboxScan
{
    /// <summary>盘面扫描：已到达但队列未知的 take。
    /// engineReady=false（无 state 文件）时说明引擎还没就绪——**新工程首次启动要装
    /// Python 依赖（约 2~3 分钟）**，此时的"等待"不是排队，文案必须区分（否则误导）。</summary>
    public static List<string> Arrived(string inbox, IEnumerable<string> knownKeys, bool engineReady = true)
    {
        var lines = new List<string>();
        if (string.IsNullOrWhiteSpace(inbox) || !Directory.Exists(inbox)) return lines;
        var note = engineReady
            ? "已到达 · 等待接管（当前任务完成后自动处理）"
            : "已到达 · 等待引擎就绪（新工程首次启动需初始化 Python 环境，约 2~3 分钟）";
        var known = new HashSet<string>(knownKeys, StringComparer.OrdinalIgnoreCase);
        try
        {
            foreach (var g in Directory.GetDirectories(inbox).OrderBy(p => p))
            {
                var gname = Path.GetFileName(g);
                // 根目录直放 take（无组）
                if (File.Exists(Path.Combine(g, "take.json")))
                {
                    if (!known.Contains(gname)) lines.Add($"[盘] {gname}   {note}");
                    continue;
                }
                foreach (var take in Directory.GetDirectories(g).OrderBy(p => p))
                {
                    var tname = Path.GetFileName(take);
                    if (!File.Exists(Path.Combine(take, "take.json"))) continue;   // 忽略杂项目录
                    var key = gname + "/" + tname;
                    if (known.Contains(key)) continue;
                    lines.Add($"[盘] {key}   {note}");
                }
            }
        }
        catch { /* inbox 不可读 → 空列表（不阻塞刷新） */ }
        return lines;
    }
}

/// <summary>强制结束：写 stop.flag + stop.intent（watchdog 见 intent 不重启）→ 杀匹配工程的编辑器。</summary>
public sealed class EngineControl
{
    private readonly IProcessQuery _query;
    private readonly Func<int, bool> _killer;

    public EngineControl(IProcessQuery query, Func<int, bool>? killer = null)
    {
        _query = query;
        _killer = killer ?? DefaultKill;
    }

    private static bool DefaultKill(int pid)
    {
        try
        {
            using var p = System.Diagnostics.Process.GetProcessById(pid);
            p.Kill();
            return true;
        }
        catch { return false; }
    }

    /// <summary>返回被杀的进程数（0 = 没有匹配的编辑器在跑）。</summary>
    public int ForceStop(string project, Paths paths)
    {
        var stamp = DateTime.Now.ToString("o");
        try
        {
            File.WriteAllText(paths.StopFlag, stamp);
            File.WriteAllText(paths.StopIntent, stamp);
        }
        catch { /* 写不进去也继续杀（Intent 缺失时 watchdog 会重启——如实返回后续可见） */ }

        var key = Path.GetFileName(project ?? "");
        if (string.IsNullOrEmpty(key)) return 0;
        var killed = 0;
        foreach (var ep in _query.GetEditors())
        {
            if (ep.CommandLine.Contains(key, StringComparison.OrdinalIgnoreCase))
                if (_killer(ep.Pid)) killed++;
        }
        return killed;
    }
}
