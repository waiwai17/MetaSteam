using System.IO;
using System.Windows;
using MhsPipeline.Core;

namespace MhsPipeline.App;

public partial class NewProjectDialog : Window
{
    public string ProjectName => NameBox.Text.Trim();
    public string TargetDir => DirBox.Text.Trim();

    public NewProjectDialog()
    {
        InitializeComponent();
        // 默认工作区：自动选"剩余空间最多的非系统盘"（不写死盘符 → 任意电脑都能用）
        var root = Workspace.DefaultRoot();
        DirBox.Text = root;
        try { Directory.CreateDirectory(root); } catch { /* 只读/无权限 → 保持文本，用户可改 */ }
        HintText.Text = $"默认工作区：{root}（自动选择剩余空间最多的非系统盘；可 [浏览] 改到任意位置）";
    }

    private void OnCreate(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrWhiteSpace(ProjectName)) return;
        DialogResult = true;
        Close();
    }
}
