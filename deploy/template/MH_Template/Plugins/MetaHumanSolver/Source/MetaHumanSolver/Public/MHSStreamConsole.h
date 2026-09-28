// MHSStreamConsole.h
// 全局进度控制台（方案 A：与单任务进度共用单一 conhost，上下分区渲染）
//
// 载体约束（本机实测，勿改回图形窗）：
//   1) UE 进程内非主线程图形窗口 DWM 拒绝渲染（句柄有效但屏幕不可见）；
//   2) 焦点切换确定性崩溃 textinputframework.dll（三崩同址，双防线无效）。
//   → 只用 conhost（系统独立进程渲染，绝对可见且安全）。
//   3) Windows 单进程只能 Attach 一个 console → 与单任务进度（FMHSProgressWindow）
//      融合：本控制台先持有 conhost，单任务窗后启动即不再拥有，永不误 FreeConsole。
//
// 数据流：独立线程 1s 读 stream_state.json + progress.json（只读，绝不回写）
//         → 组装文本 → WriteConsoleOutput 矩形整屏写入（无 cls、无闪烁）
#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS

#include <atomic>
#include <thread>

class MHSStreamConsole
{
public:
    static MHSStreamConsole& Get();

    // 启动控制台（游戏线程调用，立即返回）。重复调用幂等（已运行则仅刷新路径）。
    void Launch(const FString& InInbox, const FString& InOutputDir);
    // 停止线程并释放（仅随 UE 退出调用；[停止监听] 不调用——控制台常驻）
    void Shutdown();

    bool IsRunning() const { return bRunning.load(); }

    // 单任务窗查询：控制台接管渲染时，单任务窗不再自写 console（避免双写者撕裂）
    static bool IsConsoleTakenOver() { return Get().IsRunning(); }

private:
    void ThreadMain();
    void RenderOnce();
    void PollKeys();               // 按键轮询：T 置顶切换 / O 打开输出目录
    bool LoadStateJson(TSharedPtr<class FJsonObject>& OutRoot) const;
    bool LoadProgressJson(TSharedPtr<class FJsonObject>& OutRoot) const;

    std::thread Thread;
    std::atomic<bool> bRunning{false};
    std::atomic<bool> bStop{false};
    bool bTopmost = true;          // 置顶状态（T 切换）
    FString InboxPath;
    FString OutputPath;
    FString StatePath;
    FString ProgressPath;
    FString LastFrame;             // 上一帧文本（内容未变则跳过重绘，消除抖动）
};

#else

class MHSStreamConsole
{
public:
    static MHSStreamConsole& Get() { static MHSStreamConsole I; return I; }
    void Launch(const FString&, const FString&) {}
    void Shutdown() {}
    bool IsRunning() const { return false; }
    static bool IsConsoleTakenOver() { return false; }
};

#endif
