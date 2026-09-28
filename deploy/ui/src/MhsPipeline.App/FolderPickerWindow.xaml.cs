// 自绘文件夹选择器（进程内，不依赖 Windows 外壳对话框）。
//
// 为什么不用 OpenFolderDialog：实测点击 [浏览] 会导致 UI 线程被外壳对话框阻塞
// （窗口变灰 + "未响应"），且无法在无头/受限环境兜底。自绘对话框完全在本进程内，
// 只有目录枚举（可 try/catch），不存在挂死路径。
//
// 两种模式：
//   · 文件系统路径：根 = 驱动器列表，返回 E:\... 形式
//   · UE 内容路径：根 = <工程>/Content，返回 /Game/... 形式（面板用它填 身份导入/素材导入）

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace MhsPipeline.App;

public partial class FolderPickerWindow : Window
{
    private readonly bool _contentMode;
    private readonly string _contentRoot;      // 内容模式：<工程>/Content 的绝对路径
    private readonly string _filePattern;      // 非空 = 允许选文件（如 *.uproject），OK 返回文件路径
    private string _current;                   // 当前目录（绝对路径）

    /// <summary>用户确认后的路径（文件夹 / 文件 / /Game/… 形式）；取消为 null。</summary>
    public string? ResultPath { get; private set; }

    public FolderPickerWindow(bool contentMode, string projectUproject, string initial,
                              string filePattern = "")
    {
        InitializeComponent();
        _contentMode = contentMode;
        _filePattern = filePattern ?? "";
        if (_filePattern.Length > 0)
        {
            Title = "选择文件（" + _filePattern + "）";
            ModeHint.Text = "双击进入文件夹，单击选中文件";
        }
        var projectDir = Path.GetDirectoryName(projectUproject ?? "") ?? "";
        _contentRoot = Path.Combine(projectDir, "Content");

        if (_contentMode)
        {
            Title = "选择 UE 内容文件夹（/Game/…）";
            ModeHint.Text = "内容路径：<工程>/Content 下选择";
            RootLabel.Text = "内容根";
            RootCombo.ItemsSource = new List<string> { _contentRoot };
            RootCombo.SelectedIndex = 0;
            _current = ToFs(initial);
        }
        else
        {
            var drives = SafeDrives();
            RootCombo.ItemsSource = drives;
            RootCombo.SelectedIndex = 0;
            _current = Directory.Exists(initial) ? initial
                : (Path.GetPathRoot(initial ?? "") is { Length: > 0 } r && Directory.Exists(r) ? r
                   : (drives.FirstOrDefault() ?? "C:\\"));
        }
        if (!Directory.Exists(_current))
        {
            _current = _contentMode ? _contentRoot : SafeDrives().FirstOrDefault() ?? "C:\\";
        }
        Refresh();
    }

    private static List<string> SafeDrives()
    {
        try { return DriveInfo.GetDrives().Where(d => d.IsReady).Select(d => d.RootDirectory.FullName).ToList(); }
        catch { return new List<string> { "C:\\" }; }
    }

    // ── 路径形式转换 ──
    private string ToFs(string ueOrFs)
    {
        var p = (ueOrFs ?? "").Trim();
        if (p.StartsWith("/Game", StringComparison.OrdinalIgnoreCase))
        {
            var rel = p.Substring("/Game".Length).TrimStart('/', '\\');
            return Path.Combine(_contentRoot, rel.Replace('/', Path.DirectorySeparatorChar));
        }
        return p.Length > 0 ? p : _contentRoot;
    }

    private string ToUe(string fs)
    {
        if (string.IsNullOrEmpty(fs) || string.IsNullOrEmpty(_contentRoot)) return "";
        var root = Path.GetFullPath(_contentRoot).TrimEnd('\\', '/');
        var cur = Path.GetFullPath(fs).TrimEnd('\\', '/');
        if (!cur.StartsWith(root, StringComparison.OrdinalIgnoreCase)) return "";   // 在 Content 之外 → 无效
        var rel = cur.Substring(root.Length).TrimStart('\\', '/').Replace('\\', '/');
        return rel.Length == 0 ? "/Game" : "/Game/" + rel;
    }

    private void OnRootChanged(object sender, SelectionChangedEventArgs e)
    {
        if (RootCombo.SelectedItem is string root && Directory.Exists(root)) { _current = root; Refresh(); }
    }

    private void Refresh()
    {
        PathBox.Text = _contentMode ? (ToUe(_current) is { Length: > 0 } ue ? ue : _current) : _current;
        var items = new List<string>();
        try
        {
            if (Directory.Exists(_current))
            {
                foreach (var d in Directory.GetDirectories(_current).OrderBy(p => p))
                {
                    var name = Path.GetFileName(d);
                    if (name.StartsWith(".")) continue;
                    items.Add(name + "\\");
                }
                if (_filePattern.Length > 0)
                {
                    foreach (var f in Directory.GetFiles(_current, _filePattern).OrderBy(p => p))
                    {
                        items.Add(Path.GetFileName(f));
                    }
                }
            }
        }
        catch { /* 无权限目录 → 空列表（不阻塞） */ }
        DirList.ItemsSource = items;
        UpdateResultText();
    }

    private string? SelectedFilePath()
    {
        if (_filePattern.Length == 0 || DirList.SelectedItem is not string sel) return null;
        if (sel.EndsWith("\\")) return null;
        var full = Path.Combine(_current, sel);
        return File.Exists(full) ? full : null;
    }

    private void UpdateResultText()
    {
        var file = SelectedFilePath();
        if (file is not null) { ResultText.Text = "将填入：" + file; return; }
        var result = _contentMode ? ToUe(_current) : _current;
        if (_filePattern.Length > 0)
        {
            ResultText.Text = "请选中一个文件；当前目录：" + _current;
            return;
        }
        ResultText.Text = (result ?? "").Length == 0
            ? "⚠ 当前目录不在 <工程>/Content 内，请重新选择"
            : "将填入：" + result;
    }

    private void Enter(string name)
    {
        var sub = Path.Combine(_current, name.TrimEnd('\\', '/'));
        if (Directory.Exists(sub)) { _current = sub; Refresh(); }
    }

    private void OnDirDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (DirList.SelectedItem is string name && name.EndsWith("\\")) { Enter(name); return; }
        UpdateResultText();
    }

    private void OnDirSelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateResultText();

    private void OnUp(object sender, RoutedEventArgs e)
    {
        var parent = Path.GetDirectoryName(_current.TrimEnd('\\', '/'));
        if (string.IsNullOrEmpty(parent)) return;
        if (_contentMode && !Path.GetFullPath(parent).StartsWith(
                Path.GetFullPath(_contentRoot), StringComparison.OrdinalIgnoreCase)) return;   // 不越出 Content
        _current = parent;
        Refresh();
    }

    private void OnPathKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter) return;
        var typed = PathBox.Text.Trim();
        var fs = _contentMode ? ToFs(typed) : typed;
        if (Directory.Exists(fs)) { _current = fs; Refresh(); }
        e.Handled = true;
    }

    private void OnOk(object sender, RoutedEventArgs e)
    {
        if (_filePattern.Length > 0)
        {
            var file = SelectedFilePath();
            if (file is null) { ResultText.Text = "⚠ 请先选中一个文件"; return; }
            ResultPath = file;
            DialogResult = true;
            return;
        }
        var result = _contentMode ? ToUe(_current) : _current;
        if (string.IsNullOrEmpty(result))
        {
            ResultText.Text = "⚠ 当前目录不在 <工程>/Content 内，无法作为内容路径";
            return;
        }
        ResultPath = result;
        DialogResult = true;
    }

    private void OnCancel(object sender, RoutedEventArgs e) => DialogResult = false;
}
