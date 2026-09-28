// MHSProgressMonitor.cpp
// 解算进度监控落地实现。设计依据：《MetaHumanSolver_解算进度监控设计文档.md》（已定稿）。
//
// 三个入口（均游戏线程，由 Python UFUNCTION 调用）：
//   Begin  -> 注册 OnFrameProcessed + 启动心跳线程 + 弹原生小窗
//   SetStage -> 切换阶段显示（solve / export_as / export_ls / export_fbx）
//   End    -> 停心跳 -> 终态展示 -> 关窗 -> 移除回调 -> 写终态 json

#include "MHSProgressMonitor.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <objbase.h>   // CoInitializeEx / CoUninitialize（TSF 根因修复）
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#define LOCTEXT_NAMESPACE "MetaHumanSolverProgress"

namespace
{
    // 阶段显示名 / json 字段名（索引 = EMHSProgressStage，静态常量跨线程只读安全）
    const TCHAR* GStageDisplayNames[5] = { TEXT("深度解算中"), TEXT("导出 AnimSequence"), TEXT("导出 LevelSequence"), TEXT("导出 FBX"), TEXT("导入素材") };
    const TCHAR* GStageJsonNames[5] = { TEXT("solve"), TEXT("export_as"), TEXT("export_ls"), TEXT("export_fbx"), TEXT("import") };

    // ── 多 Pass 权重带（设计文档 11.2）──
    // 行索引 = PassCount-1，GPassBandStart[i][j] = 第 j 个 Pass 的起始权重（末项恒 1.0）：
    //   百分比 = (BandStart[pass] + (BandStart[pass+1]-BandStart[pass]) * pass内帧进度) * 100
    // 权重依据：2026-08-19 首跑实测（CD_Anims_Flint_Ul_Enter_005_5，2727 帧）——
    //   跟踪解算 211.5s / 全局最终解算 897.5s / 滤波(含音频轨) 12.1s => 18.9% / 80.1% / 1.1%。
    const double GPassBandStart[3][4] = {
        { 0.00, 1.00, 1.00, 1.00 },   // 1 个 Pass（单目视频）
        { 0.00, 0.75, 1.00, 1.00 },   // 2 个 Pass（深度 Preview：跟踪解算 + 滤波；无实测，待校准）
        { 0.00, 0.19, 0.99, 1.00 },   // 3 个 Pass（深度全量：实测校准 19 / 80 / 1）
    };
    // Pass 显示名（行索引 = PassCount-1；与 GPassBandStart 行一一对应）
    const TCHAR* GPassNames[3][3] = {
        { TEXT("解算"), TEXT(""), TEXT("") },
        { TEXT("跟踪解算"), TEXT("滤波"), TEXT("") },
        { TEXT("跟踪解算"), TEXT("全局最终解算"), TEXT("滤波") },
    };

    // 防卡死爬行参数（设计文档 11.2 保底通道）
    constexpr double GStallThresholdSeconds = 8.0;   // 帧号连续静止超此值 -> 启动爬行
    constexpr double GCreepTauSeconds = 180.0;       // 爬行时间常数（指数趋近速率）

    // 秒 -> "mm:ss" / "h:mm:ss"
    FString FormatDuration(double Seconds)
    {
        const int32 S = FMath::Max(0, FMath::RoundToInt(Seconds));
        if (S < 3600)
        {
            return FString::Printf(TEXT("%02d:%02d"), S / 60, S % 60);
        }
        return FString::Printf(TEXT("%d:%02d:%02d"), S / 3600, (S / 60) % 60, S % 60);
    }

    FString JsonEscape(const FString& In)
    {
        FString Out = In;
        Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
        Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
        return Out;
    }

    bool IsValidStage(int32 Stage)
    {
        return Stage >= 0 && Stage < 5;
    }

    // 显示列宽：中文字符占 2 列（等宽字体下与空格对齐），ASCII 占 1 列
    int32 DisplayCols(const FString& S)
    {
        int32 Cols = 0;
        for (const TCHAR& Ch : S)
        {
            Cols += (Ch < 0x80) ? 1 : 2;
        }
        return Cols;
    }

    // 填充到指定显示列宽；bRightAlign=右对齐（数字/时间列用），否则左对齐。
    // 超宽时按显示列宽截断（不截断中文字符的半个字，保证不产生乱码）。
    FString PadCol(const FString& S, int32 Width, bool bRightAlign = false)
    {
        const int32 Cols = DisplayCols(S);
        if (Cols >= Width)
        {
            FString Out;
            int32 Used = 0;
            for (const TCHAR& Ch : S)
            {
                const int32 Add = (Ch < 0x80) ? 1 : 2;
                if (Used + Add > Width)
                {
                    break;
                }
                Out += Ch;
                Used += Add;
            }
            return Out;
        }
        const FString Pad = FString::ChrN(Width - Cols, TEXT(' '));
        return bRightAlign ? Pad + S : S + Pad;
    }

    // Pass 数钳制到权重表支持的 1~3
    int32 ClampPassCount(int32 PassCount)
    {
        return FMath::Clamp(PassCount, 1, 3);
    }

    // 按当前 State 快照计算多 Pass 加权百分比（0~99 钳制；End 失败终态 / 终态 json 共用）
    double ComputeWeightedPercent(const FMHSProgressState& S)
    {
        const int32 Total = S.TotalFrames.load();
        if (Total <= 0)
        {
            return 0.0;
        }
        const int32 PassCount = ClampPassCount(S.PassCount.load());
        const int32 PassIdx = FMath::Clamp(S.PassIndex.load(), 0, PassCount - 1);
        const double* Bands = GPassBandStart[PassCount - 1];
        const double Pct = (Bands[PassIdx] + (Bands[PassIdx + 1] - Bands[PassIdx])
                            * static_cast<double>(S.CurrentFrame.load()) / static_cast<double>(Total)) * 100.0;
        return FMath::Clamp(Pct, 0.0, 99.0);
    }

    // Pass 摘要（控制台/json 终态展示行 2）：
    //   非终态："Pass1 03:12 ✓ · Pass2 进行中 · Pass3 —"
    //   终态  ：当前 Pass 记入耗时（失败于中途则未跑到的高编号 Pass 显示 —）
    // Worker（过程）与 Monitor::End（终态快照）共用。
    FString BuildPassSummaryText(int32 PassCount, int32 CurPass, const TArray<double>& CompletedDurations,
                                 double CurPassElapsed, bool bFinal)
    {
        if (PassCount <= 1)
        {
            return FString(); // 单 Pass 无边界可言，不产摘要
        }

        FString Summary;
        const int32 CurIdx = FMath::Clamp(CurPass, 0, PassCount - 1);
        for (int32 i = 0; i < PassCount; ++i)
        {
            if (i > 0)
            {
                Summary += TEXT(" · ");
            }
            if (i < CompletedDurations.Num())
            {
                Summary += FString::Printf(TEXT("Pass%d %s ✓"), i + 1, *FormatDuration(CompletedDurations[i]));
            }
            else if (i == CurIdx)
            {
                Summary += bFinal
                    ? FString::Printf(TEXT("Pass%d %s ✓"), i + 1, *FormatDuration(CurPassElapsed))
                    : FString::Printf(TEXT("Pass%d 进行中"), i + 1);
            }
            else
            {
                Summary += FString::Printf(TEXT("Pass%d —"), i + 1);
            }
        }
        return Summary;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 原生 Win32 小窗（独立线程 + 自有消息循环；创建/消息/销毁严格同一线程）
// ═══════════════════════════════════════════════════════════════════════
#if PLATFORM_WINDOWS

namespace
{
    constexpr UINT WM_MHS_UPDATE = WM_APP + 1;  // 心跳 -> 窗口：触发重绘
    constexpr UINT WM_MHS_TIMER_FINAL = 1;      // 终态停留定时器 id

    LRESULT CALLBACK MHSProgressWndProc(HWND H, UINT Msg, WPARAM W, LPARAM L)
    {
        FMHSProgressWindow* Self = nullptr;
        if (Msg == WM_NCCREATE)
        {
            Self = reinterpret_cast<FMHSProgressWindow*>(reinterpret_cast<CREATESTRUCTW*>(L)->lpCreateParams);
            ::SetWindowLongPtrW(H, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
        }
        else
        {
            Self = reinterpret_cast<FMHSProgressWindow*>(::GetWindowLongPtrW(H, GWLP_USERDATA));
        }

        switch (Msg)
        {
        case WM_MHS_UPDATE:
            // 触发重绘；标题栏同步（任务栏/置顶列表可见）
            {
                if (Self)
                {
                    Self->SyncTitle(H);
                }
                ::InvalidateRect(H, nullptr, 0);
                ::UpdateWindow(H);
            }
            return 0;

        case WM_CLOSE:
            // 运行中（非终态）：点 X = 最小化到任务栏——误关不丢监控，可从任务栏点回；
            // 终态：先停留展示（成功 1.2s / 失败 0.8s）再真正销毁，游戏线程零阻塞。
            if (Self && !Self->IsFinal())
            {
                ::ShowWindow(H, SW_MINIMIZE);
                return 0;
            }
            {
                const UINT Delay = (Self && Self->IsFinalSuccess()) ? 1200 : 800;
                ::SetTimer(H, WM_MHS_TIMER_FINAL, Delay, nullptr);
                ::InvalidateRect(H, nullptr, 0);
            }
            return 0;

        case WM_TIMER:
            if (W == WM_MHS_TIMER_FINAL)
            {
                ::KillTimer(H, WM_MHS_TIMER_FINAL);
                ::DestroyWindow(H);
            }
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        }

        return ::DefWindowProcW(H, Msg, W, L);
    }
}

void FMHSProgressWindow::Launch(const FString& InAssetName, void* InRefHwnd)
{
    if (Thread.joinable())
    {
        return; // 已在运行（重复 Begin 保护在 Monitor 层）
    }

    RefHwnd = InRefHwnd; // 线程启动前写入，happens-before 成立

    {
        std::lock_guard<std::mutex> Guard(DataLock);
        DisplayData = FMHSWindowDisplayData();
    }

    Thread = std::thread([this]()
    {
        WindowThreadMain();
        Hwnd.store(nullptr); // 创建失败/线程退出兜底清句柄
    });
    // 注：窗口创建在线程内异步完成，Launch 立即返回；Shutdown 侧对 Hwnd 就绪做了等待。
}

void FMHSProgressWindow::Shutdown()
{
    if (Thread.joinable())
    {
        // Launch 是异步创建窗口：先等窗口句柄就绪（创建失败/兜底切换则线程会自行退出），最多 2s
        for (int32 Waited = 0; Hwnd.load() == nullptr && !bConsoleMode.load() && Waited < 2000; Waited += 25)
        {
            FPlatformProcess::Sleep(0.025f);
        }

        if (void* Raw = GetHwnd())
        {
            ::PostMessageW(reinterpret_cast<HWND>(Raw), WM_CLOSE, 0, 0);
        }

        Thread.join();
        Hwnd.store(nullptr);
    }

    if (bConsoleMode.load())
    {
        // 控制台兜底模式：标题标记结束；只释放我们自己 Alloc 的控制台
        //（修复：UE 带 -stdout 启动时进程已有控制台，AllocConsole 会失败并复用它，
        //  此时 FreeConsole 会摘掉 UE 自己的 stdout，后续日志/输出全部丢失）
        ::SetConsoleTitleW(L"MetaHuman 解算监控（已结束）");
        if (bOwnedConsole.load())
        {
            ::FreeConsole();
            bOwnedConsole.store(false);
        }
        bConsoleMode.store(false);
    }
}

void FMHSProgressWindow::PostUpdate(const FMHSWindowDisplayData& InData)
{
    {
        std::lock_guard<std::mutex> Guard(DataLock);
        DisplayData = InData;
    }
    if (bConsoleMode.load())
    {
        WriteConsoleLine(InData); // console API 线程安全，心跳线程直写
        return;
    }
    if (void* Raw = GetHwnd())
    {
        ::PostMessageW(reinterpret_cast<HWND>(Raw), WM_MHS_UPDATE, 0, 0);
    }
}

void FMHSProgressWindow::PostFinal(const FMHSWindowDisplayData& InData)
{
    {
        std::lock_guard<std::mutex> Guard(DataLock);
        DisplayData = InData;
    }
    if (bConsoleMode.load())
    {
        WriteConsoleLine(InData);
        return;
    }
    if (void* Raw = GetHwnd())
    {
        ::PostMessageW(reinterpret_cast<HWND>(Raw), WM_MHS_UPDATE, 0, 0);
    }
}

void FMHSProgressWindow::WindowThreadMain()
{
    // ── 方案定案（2026-08-19 本机实测）──
    // UE 进程内非主线程图形窗口在本机存在两个环境级致命问题：
    //  1) DWM 拒绝渲染：窗口句柄有效、Alt+Tab 列表可见且标题实时更新，屏幕上却无窗口；
    //  2) 激活路径确定性崩溃：Alt+Tab/焦点切换即崩 textinputframework.dll
    //     （异常 0x675 @ 0x8354b，三崩同址；CoInitializeEx(STA)+ImmDisableIME 双防线无效）。
    // 结论：默认走控制台模式——由 conhost.exe（独立系统进程）渲染，与 UE 进程的
    // DWM/TSF/输入框架完全解耦，绝对可见且绝对安全。
    // 环境正常的机器想恢复图形小窗：把下面一行改为 WindowThreadMainGraphical(); 即可。
    EnterConsoleFallback();
}

void FMHSProgressWindow::WindowThreadMainGraphical()
{
    // ── 前置初始化（根因修复，缺一不可）──
    // 1) COM STA：textinputframework.dll（TSF）是 COM 组件，窗口显示流水线会挂接它；
    //    裸线程无 COM 上下文时确定性崩溃（实测：fault module textinputframework.dll，
    //    exception 0x675）。
    const HRESULT CoInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 2) 线程级禁用 IME：本窗口不接收任何文本输入，禁用后 TSF 不再挂接（双保险）。
    //    动态获取 imm32，避免新增链接依赖；失败说明系统无 IME，跳过即可。
    if (HMODULE Imm32 = ::GetModuleHandleW(L"imm32.dll"))
    {
        using FImmDisableIME = BOOL(WINAPI*)(DWORD);
        if (auto Fn = reinterpret_cast<FImmDisableIME>(::GetProcAddress(Imm32, "ImmDisableIME")))
        {
            Fn(::GetCurrentThreadId());
        }
    }

    const HINSTANCE Inst = ::GetModuleHandleW(nullptr);

    WNDCLASSW WC = {};
    WC.lpfnWndProc = MHSProgressWndProc;
    WC.hInstance = Inst;
    WC.lpszClassName = L"MetaHumanSolverProgressWnd";
    WC.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    if (!::RegisterClassW(&WC))
    {
        const DWORD Err = ::GetLastError();
        if (Err != ERROR_CLASS_ALREADY_EXISTS)
        {
            UE_LOG(LogTemp, Warning, TEXT("[MHS_WARN] 进度小窗 RegisterClass 失败，GLE=%u"), (uint32)Err);
            if (SUCCEEDED(CoInit)) ::CoUninitialize();
            return;
        }
    }

    // 3) DPI：不再调用线程上下文敏感的 GetDpiForSystem，先按 96 DPI 基准创建，
    //    创建成功后用窗口自身的 GetDpiForWindow（文档推荐用法）校正尺寸与位置。
    const int32 Scale0 = 96;
    const auto Sx0 = [Scale0](int32 V) { return V * Scale0 / 96; };
    const int32 WinW = Sx0(380);
    const int32 WinH = Sx0(158);

    // 定位：跟随 UE 主窗口所在显示器（多屏环境核心修正）；拿不到则跟随鼠标所在屏；
    // 都失败才退回主屏工作区。避免"UE 在副屏、小窗弹在主屏角落看不见"。
    RECT Work = {};
    const TCHAR* Where = TEXT("ue-window");
    HMONITOR Mon = RefHwnd
        ? ::MonitorFromWindow(reinterpret_cast<HWND>(RefHwnd), MONITOR_DEFAULTTONEAREST)
        : nullptr;
    if (!Mon)
    {
        POINT Cursor = {};
        ::GetCursorPos(&Cursor);
        Mon = ::MonitorFromPoint(Cursor, MONITOR_DEFAULTTONEAREST);
        Where = TEXT("cursor");
    }
    if (Mon)
    {
        MONITORINFO Mi = {};
        Mi.cbSize = sizeof(Mi);
        if (::GetMonitorInfoW(Mon, &Mi))
        {
            Work = Mi.rcWork;
        }
    }
    if (Work.right == 0 && Work.bottom == 0)
    {
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &Work, 0);
        Where = TEXT("primary");
    }
    int32 X = static_cast<int32>(Work.right) - WinW - Sx0(28);
    int32 Y = static_cast<int32>(Work.bottom) - WinH - Sx0(28);

    HWND H = ::CreateWindowExW(
        // layered：窗口表面直接交 DWM 合成，规避 TOOLWINDOW/NOACTIVATE 类窗口的
        // "隐身（cloaking）"行为（句柄有效、IsWindowVisible 真、就是不渲染）。
        // 去掉 TOOLWINDOW/NOACTIVATE：任务栏可见、可 Alt+Tab，用户永远找得到。
        WS_EX_TOPMOST | WS_EX_LAYERED,
        L"MetaHumanSolverProgressWnd",
        L"MetaHuman 解算监控",
        WS_POPUP | WS_VISIBLE,
        X, Y, WinW, WinH,
        nullptr, nullptr, Inst, this);

    if (!H)
    {
        const DWORD Err = ::GetLastError();
        UE_LOG(LogTemp, Warning, TEXT("[MHS_WARN] 进度小窗创建失败，GLE=%u，切换控制台兜底"), (uint32)Err);
        EnterConsoleFallback();
        return;
    }
    // 不透明 layered（LWA_ALPHA, 255）——与 WM_PAINT 自绘兼容
    ::SetLayeredWindowAttributes(H, 0, 255, LWA_ALPHA);

    // 按窗口真实 DPI 校正尺寸与位置（高 DPI 屏上窗口不至于偏小）
    if (const UINT Dpi = ::GetDpiForWindow(H); Dpi > 0 && Dpi != 96)
    {
        const auto Sx = [Dpi](int32 V) { return V * static_cast<int32>(Dpi) / 96; };
        const int32 RealW = Sx(380);
        const int32 RealH = Sx(158);
        X = static_cast<int32>(Work.right) - RealW - Sx(28);
        Y = static_cast<int32>(Work.bottom) - RealH - Sx(28);
        ::SetWindowPos(H, nullptr, X, Y, RealW, RealH, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    // 显式显示
    ::ShowWindow(H, SW_SHOWNOACTIVATE);

    // ── 可见性自检（把"看不看得见"变成日志里可验证的事实）──
    const bool bVisible = (::IsWindowVisible(H) != 0);
    bool bCloaked = false;
    if (HMODULE Dwm = ::GetModuleHandleW(L"dwmapi.dll"))
    {
        using FGetAttr = HRESULT(WINAPI*)(HWND, DWORD, void*, DWORD);
        if (auto Fn = reinterpret_cast<FGetAttr>(::GetProcAddress(Dwm, "DwmGetWindowAttribute")))
        {
            DWORD Cloak = 0;
            if (S_OK == Fn(H, 13 /*DWMWA_CLOAKED*/, &Cloak, sizeof(Cloak)))
            {
                bCloaked = (Cloak != 0);
            }
        }
    }
    UE_LOG(LogTemp, Log, TEXT("[MHS_LOG] 进度小窗已创建: 0x%p @(%d,%d) %dx%d 定位=%s 自检[可见=%d 隐身=%d]"),
        H, X, Y, WinW, WinH, Where, bVisible ? 1 : 0, bCloaked ? 1 : 0);

    if (!bVisible || bCloaked)
    {
        UE_LOG(LogTemp, Warning, TEXT("[MHS_WARN] 小窗被系统判定不可见/隐身，切换控制台兜底显示"));
        ::DestroyWindow(H);
        Hwnd.store(nullptr);
        EnterConsoleFallback();
        return;
    }
    Hwnd.store(H);

    // 消息循环（GetMessage 阻塞式，不占 CPU；窗口销毁 -> WM_DESTROY -> PostQuitMessage -> 退出）
    MSG Msg;
    while (::GetMessageW(&Msg, nullptr, 0, 0) > 0)
    {
        ::TranslateMessage(&Msg);
        ::DispatchMessageW(&Msg);
    }

    if (SUCCEEDED(CoInit)) ::CoUninitialize();
}

void FMHSProgressWindow::EnterConsoleFallback()
{
    // 控制台窗口由系统 conhost 渲染，DWM 无法隐身——绝对可见兜底。
    // AllocConsole 失败（ERROR_ACCESS_DENIED）通常意味着进程已有控制台（-stdout 无头模式），
    // 此时直接复用现有控制台写进度，天然兼容无头场景；且 Shutdown 绝不 FreeConsole
    // （bOwnedConsole=false），避免摘掉 UE 自己的 stdout。
    bConsoleMode.store(true);
    bOwnedConsole.store(::AllocConsole() != 0);
    if (bOwnedConsole.load())
    {
        if (HWND Con = ::GetConsoleWindow())
        {
            ::SetConsoleTitleW(L"MetaHuman 解算监控");

            // 缓冲区先加宽到 160 列再拉大窗口：MoveWindow 只改窗口不改缓冲区，
            // 默认 80 列缓冲装不下两行渲染的行宽（~150 字符），会折行破坏 {0,0} 重写定位。
            if (HANDLE Out = ::GetStdHandle(STD_OUTPUT_HANDLE); Out && Out != INVALID_HANDLE_VALUE)
            {
                CONSOLE_SCREEN_BUFFER_INFO Info = {};
                if (::GetConsoleScreenBufferInfo(Out, &Info) && Info.dwSize.X < 160)
                {
                    COORD NewSize = { 160, Info.dwSize.Y };
                    ::SetConsoleScreenBufferSize(Out, NewSize);
                }
            }
            ::MoveWindow(Con, 40, 40, 1080, 260, 1);
            ::SetWindowPos(Con, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    UE_LOG(LogTemp, Log, TEXT("[MHS_LOG] 进度显示已切换控制台模式（标题=百分比+批次位置，两行详情）"));
}

void FMHSProgressWindow::WriteConsoleLine(const FMHSWindowDisplayData& D)
{
    // ── 标题（任务栏也可见）：MetaHuman 解算中 45.3% [3/12] ──
    FString Title;
    if (D.bFinal)
    {
        Title = D.bSuccess ? TEXT("MetaHuman 解算完成 100%") : TEXT("MetaHuman 解算失败");
    }
    else if (D.Stage == 0)
    {
        Title = FString::Printf(TEXT("MetaHuman 解算中 %.1f%%"), D.Percent);
    }
    else
    {
        Title = FString::Printf(TEXT("MetaHuman %s"), IsValidStage(D.Stage) ? GStageDisplayNames[D.Stage] : TEXT("处理中"));
    }
    if (D.BatchTotal > 0 && !D.bFinal)
    {
        Title += FString::Printf(TEXT(" [%d/%d]"), FMath::Clamp(D.BatchIndex, 1, D.BatchTotal), D.BatchTotal);
    }
    if (Title.Len() > 150)
    {
        Title = Title.Left(150);
    }
    ::SetConsoleTitleW(*Title);

    // ── 正文两行（光标归位 {0,0} 重写；批处理适配：行 1 带批次位置 + 段名 + Pass 信息） ──
    if (HANDLE Out = ::GetStdHandle(STD_OUTPUT_HANDLE); Out && Out != INVALID_HANDLE_VALUE)
    {
        CONSOLE_SCREEN_BUFFER_INFO Info = {};
        if (!::GetConsoleScreenBufferInfo(Out, &Info))
        {
            return;
        }
        const COORD Origin = { 0, 0 };
        ::SetConsoleCursorPosition(Out, Origin);

        // 批次位置前缀（无批次上下文时为空）
        const FString Pos = (D.BatchTotal > 0)
            ? FString::Printf(TEXT("[%d/%d] "), FMath::Clamp(D.BatchIndex, 1, D.BatchTotal), D.BatchTotal)
            : TEXT("");
        // 段名截断保护（超长 shot 名不撑爆行宽）
        FString Label = D.Label;
        if (Label.Len() > 40)
        {
            Label = Label.Left(37) + TEXT("...");
        }

        // 行 1：批次位置 · 段名 · Pass/阶段 · 百分比 · 帧 · 耗时 · 剩余
        FString Line1;
        if (D.bFinal)
        {
            Line1 = FString::Printf(TEXT("%s%s%s · 总耗时 %s%s"),
                *Pos, *Label,
                D.bSuccess ? TEXT(" 完成") : TEXT(" 失败"),
                *FormatDuration(D.Elapsed),
                D.bSuccess ? TEXT("") : TEXT("（详见日志）"));
        }
        else if (D.TaskTotal > 0)
        {
            // 批次导入任务进度：x/y take · 并发n · 已耗 · 剩余（C++ 并发调度逐 take 更新）
            const FString TaskEtaTxt = (D.TaskEta >= 0.0) ? FormatDuration(D.TaskEta) : TEXT("--:--");
            Line1 = FString::Printf(TEXT("%s%s · %d/%d take · 并发%d · 已耗 %s · 剩余 %s"),
                *Pos, *Label, D.TaskCurrent, D.TaskTotal, FMath::Max(D.TaskConcurrency, 1),
                *FormatDuration(D.Elapsed), *TaskEtaTxt);
        }
        else if (D.Stage == 0 && D.Total > 0)
        {
            // 单行固定列宽对齐：批次[i/N] · 数据名 · 阶段N(Pass编号) · 帧(右对齐) · 已耗/剩余(右对齐)
            const int32 PassIdx = FMath::Clamp(D.Pass, 0, D.PassCount - 1);
            const FString StageTxt = FString::Printf(TEXT("阶段%d"), PassIdx + 1);
            const FString FramesTxt = FString::Printf(TEXT("%d/%d帧"), D.Cur, D.Total);
            const FString TimeTxt = (D.Eta >= 0.0)
                ? FString::Printf(TEXT("%s/%s"), *FormatDuration(D.Elapsed), *FormatDuration(D.Eta))
                : FString::Printf(TEXT("%s/--:--"), *FormatDuration(D.Elapsed));
            Line1 = PadCol(Pos, 8) + PadCol(Label, 24)
                  + PadCol(StageTxt, 8)
                  + PadCol(FramesTxt, 13, true)
                  + PadCol(TimeTxt, 15, true);
        }
        else if (D.Stage == 0)
        {
            Line1 = FString::Printf(TEXT("%s%s · 解算准备中 · 已耗 %s"),
                *Pos, *Label, *FormatDuration(D.Elapsed));
        }
        else
        {
            Line1 = FString::Printf(TEXT("%s%s · %s · 已耗 %s"),
                *Pos, *Label,
                IsValidStage(D.Stage) ? GStageDisplayNames[D.Stage] : TEXT("处理中"),
                *FormatDuration(D.Elapsed));
        }

        // 行 2：批次剩余预估 + Pass 摘要（单 Pass 时不显示 Pass 摘要）
        FString Line2;
        if (D.TaskTotal > 0 && !D.TaskActiveNames.IsEmpty())
        {
            Line2 = TEXT("转换中: ") + D.TaskActiveNames;
        }
        else if (D.BatchTotal > 0)
        {
            const FString BatchEtaTxt = (D.BatchEta >= 0.0) ? FormatDuration(D.BatchEta) : TEXT("估算中");
            Line2 = FString::Printf(TEXT("批次剩余预估 %s"), *BatchEtaTxt);
            if (!D.PassSummary.IsEmpty())
            {
                Line2 += TEXT(" · ") + D.PassSummary;
            }
        }
        else
        {
            Line2 = D.PassSummary;
        }

        // 截断 + 填充到固定宽度：清掉上一 tick 的残留字符（中文占 2 列，144 字符 + 少量中文 < 160 列缓冲）
        const auto FitLine = [](FString S)
        {
            if (S.Len() > 144)
            {
                S = S.Left(144);
            }
            else if (S.Len() < 144)
            {
                S += FString::ChrN(144 - S.Len(), TEXT(' '));
            }
            return S;
        };
        Line1 = FitLine(MoveTemp(Line1));
        Line2 = FitLine(MoveTemp(Line2));

        const FString Full = Line1 + TEXT("\r\n") + Line2;
        DWORD Written = 0;
        ::WriteConsoleW(Out, *Full, Full.Len(), &Written, nullptr);
    }
}

void FMHSProgressWindow::SyncTitle(HWND H)
{
    FMHSWindowDisplayData D;
    {
        std::lock_guard<std::mutex> Guard(DataLock);
        D = DisplayData;
    }

    wchar_t Buf[128];
    if (D.bFinal)
    {
        if (D.bSuccess)
        {
            swprintf_s(Buf, L"MetaHuman 解算完成 · 100%%");
        }
        else
        {
            swprintf_s(Buf, L"MetaHuman 解算失败");
        }
    }
    else if (IsValidStage(D.Stage))
    {
        swprintf_s(Buf, L"MetaHuman 解算中 %.1f%%", D.Percent);
    }
    else
    {
        swprintf_s(Buf, L"MetaHuman 解算中");
    }
    ::SetWindowTextW(H, Buf);
}

bool FMHSProgressWindow::IsFinalSuccess() const
{
    std::lock_guard<std::mutex> Guard(DataLock);
    return DisplayData.bFinal && DisplayData.bSuccess;
}

bool FMHSProgressWindow::IsFinal() const
{
    std::lock_guard<std::mutex> Guard(DataLock);
    return DisplayData.bFinal;
}

void FMHSProgressWindow::Paint(HWND H)
{
    PAINTSTRUCT PS;
    HDC DC = ::BeginPaint(H, &PS);
    if (!DC)
    {
        return;
    }

    // 数据快照（互斥锁内拷贝，窗口线程独占绘制）
    FMHSWindowDisplayData D;
    {
        std::lock_guard<std::mutex> Guard(DataLock);
        D = DisplayData;
    }

    RECT RC;
    ::GetClientRect(H, &RC);
    const int32 W = RC.right;
    const int32 Hgt = RC.bottom;

    const UINT Dpi = ::GetDpiForWindow(H);
    const int32 Scale = (Dpi > 0) ? static_cast<int32>(Dpi) : 96;
    const auto Sx = [Scale](int32 V) { return V * Scale / 96; };

    // 双缓冲
    HDC Mem = ::CreateCompatibleDC(DC);
    HBITMAP Bmp = ::CreateCompatibleBitmap(DC, W, Hgt);
    HGDIOBJ OldBmp = ::SelectObject(Mem, Bmp);

    // 背景
    HBRUSH Bg = ::CreateSolidBrush(RGB(18, 18, 22));
    ::FillRect(Mem, &RC, Bg);
    ::DeleteObject(Bg);

    // 颜色
    const COLORREF ClrMain = RGB(228, 228, 232);
    const COLORREF ClrDim = RGB(150, 150, 158);
    const COLORREF ClrBarBg = RGB(52, 52, 60);
    const COLORREF ClrBar = D.bFinal
        ? (D.bSuccess ? RGB(76, 175, 80) : RGB(230, 80, 80))
        : RGB(42, 110, 220);

    ::SetBkMode(Mem, TRANSPARENT);

    // 字体工厂（局部创建销毁：2Hz 频率开销可忽略，换取零泄漏）
    const auto MakeFont = [Sx](int32 PtHeight, bool bBold) -> HFONT
    {
        return ::CreateFontW(
            -Sx(PtHeight), 0, 0, 0,
            bBold ? FW_SEMIBOLD : FW_NORMAL,
            0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
    };

    // 第 1 行：阶段名（成功/失败时替换为终态文本）
    {
        HFONT F = MakeFont(13, true);
        HGDIOBJ Old = ::SelectObject(Mem, F);

        FString Line;
        if (D.bFinal)
        {
            Line = D.bSuccess ? TEXT("✔ 解算完成") : TEXT("✘ 解算失败（详见日志）");
        }
        else
        {
            Line = IsValidStage(D.Stage) ? GStageDisplayNames[D.Stage] : TEXT("处理中");
        }
        ::SetTextColor(Mem, D.bFinal ? (D.bSuccess ? RGB(76, 175, 80) : RGB(230, 80, 80)) : ClrMain);
        RECT R1 = { Sx(16), Sx(12), W - Sx(16), Sx(12) + Sx(22) };
        ::DrawTextW(Mem, *Line, Line.Len(), &R1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        ::SelectObject(Mem, Old);
        ::DeleteObject(F);
    }

    // 第 2 行：大号百分比（导出阶段显示"进行中"走秒提示）
    {
        HFONT F = MakeFont(40, true);
        HGDIOBJ Old = ::SelectObject(Mem, F);

        FString Pct;
        if (D.bFinal)
        {
            Pct = D.bSuccess ? TEXT("100%") : FString::Printf(TEXT("%.1f%%"), D.Percent);
        }
        else if (D.Stage == 0 && D.Total > 0)
        {
            Pct = FString::Printf(TEXT("%.1f%%"), D.Percent);
        }
        else
        {
            Pct = TEXT("…");
        }
        ::SetTextColor(Mem, ClrMain);
        RECT R2 = { Sx(16), Sx(34), W - Sx(16), Sx(34) + Sx(56) };
        ::DrawTextW(Mem, *Pct, Pct.Len(), &R2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 右侧对齐帧计数
        FString Frames;
        if (!D.bFinal && D.Stage == 0 && D.Total > 0)
        {
            Frames = FString::Printf(TEXT("%d / %d 帧"), D.Cur, D.Total);
        }
        ::SetTextColor(Mem, ClrDim);
        RECT R2b = { Sx(16), Sx(34), W - Sx(16), Sx(34) + Sx(56) };
        ::DrawTextW(Mem, *Frames, Frames.Len(), &R2b, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        ::SelectObject(Mem, Old);
        ::DeleteObject(F);
    }

    // 第 3 行：进度条
    {
        const int32 BarY = Sx(96);
        const int32 BarH = Sx(9);
        const int32 BarX = Sx(16);
        const int32 BarW = W - Sx(32);

        RECT RFull = { BarX, BarY, BarX + BarW, BarY + BarH };
        HBRUSH B1 = ::CreateSolidBrush(ClrBarBg);
        ::FillRect(Mem, &RFull, B1);
        ::DeleteObject(B1);

        const double PctClamped = FMath::Clamp(D.bFinal && D.bSuccess ? 100.0 : D.Percent, 0.0, 100.0);
        const int32 FillW = static_cast<int32>(BarW * PctClamped / 100.0);
        if (FillW > 0)
        {
            RECT RFill = { BarX, BarY, BarX + FillW, BarY + BarH };
            HBRUSH B2 = ::CreateSolidBrush(ClrBar);
            ::FillRect(Mem, &RFill, B2);
            ::DeleteObject(B2);
        }
    }

    // 第 4 行：已耗时 · 预计剩余
    {
        HFONT F = MakeFont(12, false);
        HGDIOBJ Old = ::SelectObject(Mem, F);

        FString Info;
        if (D.bFinal)
        {
            Info = FString::Printf(TEXT("总耗时 %s"), *FormatDuration(D.Elapsed));
        }
        else
        {
            const FString EtaTxt = (D.Eta >= 0.0)
                ? FormatDuration(D.Eta)
                : TEXT("估算中…");
            Info = FString::Printf(TEXT("已耗时 %s · 预计剩余 %s"), *FormatDuration(D.Elapsed), *EtaTxt);
        }
        ::SetTextColor(Mem, ClrDim);
        RECT R4 = { Sx(16), Sx(114), W - Sx(16), Sx(114) + Sx(20) };
        ::DrawTextW(Mem, *Info, Info.Len(), &R4, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        ::SelectObject(Mem, Old);
        ::DeleteObject(F);
    }

    // 提交双缓冲
    ::BitBlt(DC, 0, 0, W, Hgt, Mem, 0, 0, SRCCOPY);
    ::SelectObject(Mem, OldBmp);
    ::DeleteObject(Bmp);
    ::DeleteDC(Mem);

    ::EndPaint(H, &PS);
}

#endif // PLATFORM_WINDOWS

// ═══════════════════════════════════════════════════════════════════════
// 心跳线程
// ═══════════════════════════════════════════════════════════════════════

FMHSProgressWorker::FMHSProgressWorker(FMHSProgressState& InState, FMHSProgressWindow& InWindow)
    : State(InState)
    , Window(InWindow)
    , PassStartTime(FPlatformTime::Seconds()) // Pass 1 计时起点（与 start_pipeline 间隔为毫秒级，可忽略）
{
}

uint32 FMHSProgressWorker::Run()
{
    while (!State.bShouldStop.load(std::memory_order_relaxed))
    {
        FPlatformProcess::Sleep(0.5f); // 500ms 节流（设计文档 7.4）

        if (!State.bActive.load(std::memory_order_relaxed))
        {
            continue;
        }

        TickOnce();
    }
    return 0;
}

void FMHSProgressWorker::TickOnce()
{
    const double Now = FPlatformTime::Seconds();

    // Poke UE 主窗口（WM_NULL 无害消息，仅唤醒消息泵）：游戏线程被解算 Exec 长阻塞后
    // Windows 将窗口打入挂起状态、主循环陷入消息等待停滞（ AsyncTask/timer 排队等 tick，
    // 表现为"点击面板才推进下一批"）。心跳线程每 500ms 程序化唤醒——等价于持续轻点窗口。
    // 仅在 UE 进程有主窗口时（无头模式 Hwnd 为空则跳过）。
    if (void* Hwnd = State.MainWindowHwnd.load(std::memory_order_relaxed))
    {
        ::PostMessageW(reinterpret_cast<HWND>(Hwnd), WM_NULL, 0, 0);
    }

    FMHSWindowDisplayData D;
    D.Cur = State.CurrentFrame.load();
    D.Total = State.TotalFrames.load();
    D.Elapsed = Now - State.StartTime.load();
    D.Stage = State.Stage.load();
    D.Pass = State.PassIndex.load();
    D.PassCount = ClampPassCount(State.PassCount.load());
    D.BatchIndex = State.BatchIndex.load();
    D.BatchTotal = State.BatchTotal.load();
    D.BatchEta = State.BatchEtaSeconds.load();
    D.Label = State.AssetName;
    D.TaskCurrent = State.TaskCurrent.load();
    D.TaskTotal = State.TaskTotal.load();
    D.TaskConcurrency = State.TaskConcurrency.load();
    D.TaskEta = State.TaskEtaSeconds.load(); // C++ 调度在 take 完成时按"已完成纯耗时均值×剩余数"更新，转换期间保持不变
    if (D.TaskTotal > 0)
    {
        std::lock_guard<std::mutex> Lock(State.TaskActiveMutex);
        D.TaskActiveNames = State.TaskActiveNames;
    }

    // ── Pass 边界观测：打 Pass 耗时日志（首跑即得权重校准数据）+ Pass 计时复位 ──
    if (D.Pass != LastSeenPass)
    {
        if (D.Pass > LastSeenPass && LastSeenPass >= 0)
        {
            const double Dur = Now - PassStartTime;
            CompletedPassDurations.Add(Dur);
            const int32 PrevIdx = FMath::Clamp(LastSeenPass, 0, D.PassCount - 1);
            // 诊断字段：累计广播数 + 最近回绕触发现场（还原各 Stage 帧广播行为）
            UE_LOG(LogTemp, Log, TEXT("[MHS_LOG] Pass %d/%d（%s）完成 · 耗时 %s · 累计广播 %d 帧 · 回绕触发帧 %d @+%s"),
                LastSeenPass + 1, D.PassCount, GPassNames[D.PassCount - 1][PrevIdx], *FormatDuration(Dur),
                State.BroadcastCount.load(std::memory_order_relaxed),
                State.LastWrapTriggerFrame.load(std::memory_order_relaxed),
                *FormatDuration(State.LastWrapTime.load(std::memory_order_relaxed)));
        }
        PassStartTime = Now;
        LastSeenPass = D.Pass;
        StallSeconds = 0.0; // 新 Pass 帧号从 0 重新出发，静止计时复位
    }

    // ── 主通道：帧驱动多 Pass 加权百分比 ──
    double FramePct = 0.0;
    if (D.Stage == 0 && D.Total > 0)
    {
        const int32 PassIdx = FMath::Clamp(D.Pass, 0, D.PassCount - 1);
        const double* Bands = GPassBandStart[D.PassCount - 1];
        FramePct = (Bands[PassIdx] + (Bands[PassIdx + 1] - Bands[PassIdx])
                    * static_cast<double>(D.Cur) / static_cast<double>(D.Total)) * 100.0;
    }

    // ── 保底通道：防卡死爬行（帧号 >8s 未推进 -> 向当前 Pass 权重带末端指数趋近） ──
    if (D.Cur == LastTickCur)
    {
        StallSeconds += 0.5; // 心跳周期
    }
    else
    {
        StallSeconds = 0.0;
    }
    LastTickCur = D.Cur;

    if (D.Stage == 0 && D.Total > 0 && StallSeconds > GStallThresholdSeconds)
    {
        if (!bCreeping)
        {
            bCreeping = true;
            CreepPercent = FMath::Max(CreepPercent, DisplayHighWater); // 以当前显示值为起点爬
        }
        // 帧进度反超爬行值时抬升爬行底线（显示取 max，单调不回退）
        CreepPercent = FMath::Max(CreepPercent, DisplayHighWater);
        const int32 PassIdx = FMath::Clamp(D.Pass, 0, D.PassCount - 1);
        // 目标：当前 Pass 权重带末端内收 0.5%，且不超过 98.5%（永不顶到 99 钳制线）
        const double Target = FMath::Min(GPassBandStart[D.PassCount - 1][PassIdx + 1] * 100.0 - 0.5, 98.5);
        if (Target - 0.05 > CreepPercent)
        {
            // 指数趋近（τ=3min）+ 最小步长 0.005%/tick：每 tick 必有位移，又永不触顶
            const double Step = FMath::Max((Target - CreepPercent) * (0.5 / GCreepTauSeconds), 0.005);
            CreepPercent = FMath::Min(CreepPercent + Step, Target - 0.05);
        }
    }
    else
    {
        bCreeping = false; // 帧恢复推进 / 离开解算阶段：爬行暂停（CreepPercent 保留供 max 合成）
    }

    // ── 显示合成：max(帧驱动, 爬行) + 高水位单调，钳制 0~99（100% 只由 End 硬触发，设计文档 10.2） ──
    double Computed = (D.Stage == 0 && D.Total > 0) ? FramePct : 0.0;
    Computed = FMath::Max(Computed, CreepPercent);
    Computed = FMath::Clamp(Computed, 0.0, 99.0);
    DisplayHighWater = FMath::Max(DisplayHighWater, Computed);
    D.Percent = DisplayHighWater;

    // ETA：已耗时/加权进度 外推 + EMA 平滑（设计文档 13.3；加权后不再卡 99% 期间失真）
    const double InstEta = (D.Stage == 0 && D.Cur > 0 && D.Percent > 1.0)
        ? (D.Elapsed / D.Percent) * (100.0 - D.Percent)
        : -1.0;
    EmaEta = (InstEta < 0.0) ? -1.0
        : (EmaEta < 0.0 ? InstEta : EmaEta * 0.7 + InstEta * 0.3);
    D.Eta = EmaEta;

    D.PassSummary = BuildPassSummaryText(D.PassCount, D.Pass, CompletedPassDurations, Now - PassStartTime, D.bFinal);

    WriteJson(D);
    Window.PostUpdate(D);
}

void FMHSProgressWorker::WriteJson(const FMHSWindowDisplayData& Data) const
{
    // 原子写：临时文件 + 替换（读方永远看到完整 json，设计文档 9）
    const FString StateStr = (Data.Stage == static_cast<int32>(EMHSProgressStage::Solve)) ? TEXT("solving")
        : (Data.Stage == static_cast<int32>(EMHSProgressStage::Import)) ? TEXT("import")
        : TEXT("export");
    const FString StageStr = IsValidStage(Data.Stage) ? GStageJsonNames[Data.Stage] : TEXT("unknown");
    const double EtaVal = (Data.Eta >= 0.0) ? Data.Eta : -1.0;
    const double BatchEtaVal = (Data.BatchEta >= 0.0) ? Data.BatchEta : -1.0;

    const FString Json = FString::Printf(
        TEXT("{\n")
        TEXT("  \"asset_name\": \"%s\",\n")
        TEXT("  \"state\": \"%s\",\n")
        TEXT("  \"stage\": \"%s\",\n")
        TEXT("  \"pass\": %d,\n")
        TEXT("  \"pass_count\": %d,\n")
        TEXT("  \"current_frame\": %d,\n")
        TEXT("  \"total_frames\": %d,\n")
        TEXT("  \"percent\": %.1f,\n")
        TEXT("  \"elapsed_seconds\": %d,\n")
        TEXT("  \"eta_seconds\": %.0f,\n")
        TEXT("  \"batch_index\": %d,\n")
        TEXT("  \"batch_total\": %d,\n")
        TEXT("  \"batch_eta_seconds\": %.0f,\n")
        TEXT("  \"updated_at\": \"%s\"\n")
        TEXT("}"),
        *JsonEscape(State.AssetName),
        *StateStr,
        *StageStr,
        FMath::Clamp(Data.Pass, 0, Data.PassCount - 1) + 1, // 1-based（人读）
        Data.PassCount,
        Data.Cur,
        Data.Total,
        Data.Percent,
        static_cast<int32>(Data.Elapsed),
        EtaVal,
        Data.BatchIndex,
        Data.BatchTotal,
        BatchEtaVal,
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));

    const FString Tmp = State.ProgressFilePath + TEXT(".tmp");
    if (FFileHelper::SaveStringToFile(Json, *Tmp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        IFileManager::Get().Move(*State.ProgressFilePath, *Tmp, /*bReplace=*/true);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 监控单例
// ═══════════════════════════════════════════════════════════════════════

FMHSProgressMonitor& FMHSProgressMonitor::Get()
{
    static FMHSProgressMonitor Instance;
    return Instance;
}

void FMHSProgressMonitor::Begin(UMetaHumanPerformance* InPerf, const FString& InAssetName, int32 InExpectedPasses)
{
    if (!InPerf)
    {
        return;
    }

    // 0) 重复 Begin 保护：先强制 End（设计文档 7.2）
    if (State.bActive.load())
    {
        End(false);
    }

    // 1) 初始化状态（字符串在两个线程启动前写好，happens-before 成立）
    //    注意：批次上下文（BatchIndex/Total/Eta）跨段持久，这里不重置
    //    （batch_runner 每段启动前刷新、批末清空，设计文档 11.3）。
    State.AssetName = InAssetName;
    State.ProgressFilePath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/progress.json");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(State.ProgressFilePath), true);

    State.StartTime.store(FPlatformTime::Seconds());
    State.CurrentFrame.store(0);
    State.TotalFrames.store(0);
    State.FrameLowerBound.store(0);
    State.Stage.store(static_cast<int32>(EMHSProgressStage::Solve));
    State.PassIndex.store(0);
    State.PassCount.store(ClampPassCount(InExpectedPasses));
    State.BroadcastCount.store(0);
    State.LastWrapTriggerFrame.store(-1);
    State.LastWrapTime.store(0.0);

    // 1.5) 重置任务进度状态（导入段残留必须清，否则解算段行 1 仍显示 N/M take + 并发 + 剩余）
    State.TaskCurrent.store(0);
    State.TaskTotal.store(0);          // 0 = 无任务进度（小窗行 1 走解算/终态分支，不再走 TaskTotal>0 分支）
    State.TaskConcurrency.store(0);
    State.TaskEtaSeconds.store(-1.0);
    {
        std::lock_guard<std::mutex> Lock(State.TaskActiveMutex);
        State.TaskActiveNames.Empty();
    }

    // 2) 游戏线程读一次帧范围（Begin 时可能尚未初始化 -> 首帧回调动态修正，设计文档 7.3）
    const TRange<FFrameNumber>& Range = InPerf->GetProcessingLimitFrameRange();
    const int32 Total = Range.GetUpperBoundValue().Value - Range.GetLowerBoundValue().Value;
    if (Total > 0)
    {
        State.TotalFrames.store(Total);
        State.FrameLowerBound.store(Range.GetLowerBoundValue().Value);
    }

    WeakPerf = InPerf;

    // 3) 注册逐帧回调（游戏线程调用栈内触发；纪律：只做 atomic store + 首帧一次性修正）
    TWeakObjectPtr<UMetaHumanPerformance> Weak = WeakPerf;
    FrameHandle = InPerf->OnFrameProcessed().AddLambda([this, Weak](int32 InFrame)
    {
        // 帧号换算：sequencer 帧号 - 下界 = 0-based 动画帧号（MetaHumanPerformance.cpp:1994）
        int32 Normalized = InFrame - State.FrameLowerBound.load(std::memory_order_relaxed);
        if (Normalized < 0)
        {
            Normalized = InFrame; // 防御：下界未就绪
        }

        // Pass 回绕检测（设计文档 11.2）：每个 Pass 内广播帧号严格 0->N-1 递增
        // （FaceTrackerPostProcessingNode.cpp:76/120 核实），收到比当前水位更小的帧号 = 进入新 Pass。
        State.BroadcastCount.fetch_add(1, std::memory_order_relaxed); // 诊断：累计广播数（atomic，零开销）
        int32 Prev = State.CurrentFrame.load(std::memory_order_relaxed); // 非 const：CAS 需可写引用
        if (Normalized < Prev)
        {
            State.PassIndex.fetch_add(1, std::memory_order_relaxed);
            State.CurrentFrame.store(Normalized, std::memory_order_relaxed);
            // 诊断：回绕触发现场（心跳日志带出，不在此打日志——回调零日志纪律）
            State.LastWrapTriggerFrame.store(Normalized, std::memory_order_relaxed);
            State.LastWrapTime.store(FPlatformTime::Seconds() - State.StartTime.load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
        }
        else if (Normalized > Prev)
        {
            // 同 Pass 内推进（游戏线程唯一写者，宽松原子即可；CAS 兼容防御性多写）
            while (Prev < Normalized && !State.CurrentFrame.compare_exchange_weak(Prev, Normalized))
            {
            }
        }
        // Normalized == Prev：同帧重复广播，忽略

        // 首帧动态修正总帧数（游戏线程，UObject 访问安全）
        if (State.TotalFrames.load() == 0 && State.bActive.load())
        {
            if (UMetaHumanPerformance* P = Weak.Get())
            {
                const TRange<FFrameNumber>& R = P->GetProcessingLimitFrameRange();
                const int32 T = R.GetUpperBoundValue().Value - R.GetLowerBoundValue().Value;
                if (T > 0)
                {
                    State.FrameLowerBound.store(R.GetLowerBoundValue().Value);
                    State.TotalFrames.store(T);
                }
            }
        }
    });

    State.bShouldStop.store(false);
    State.bActive.store(true);

    // 4) 启动窗口（立即可见）+ 心跳线程
    //    小窗跟随 UE 主窗口所在显示器（多屏修正）：取当前活动顶层 Slate 窗口的原生句柄。
    void* UeMainHwnd = nullptr;
    if (FSlateApplication::IsInitialized())
    {
        const TSharedPtr<SWindow> TopWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
        if (TopWindow.IsValid() && TopWindow->GetNativeWindow().IsValid())
        {
            UeMainHwnd = TopWindow->GetNativeWindow()->GetOSWindowHandle();
        }
    }
    State.MainWindowHwnd.store(UeMainHwnd);  // 心跳线程 poke 主窗口（防消息泵停滞）
    Window.Launch(InAssetName, UeMainHwnd);

    Worker = new FMHSProgressWorker(State, Window);
    WorkerThread = FRunnableThread::Create(Worker, TEXT("MHSProgressHeartbeat"), 0, TPri_Normal);

    UE_LOG(LogTemp, Log, TEXT("[MHS_LOG] 进度监控已启动: %s（预期 %d 个 Pass）"), *InAssetName, ClampPassCount(InExpectedPasses));
}

void FMHSProgressMonitor::BeginTask(const FString& InTaskName, int32 InStage)
{
    // 无 perf 的任务级监控（素材导入段）：复用 Begin 的窗口/心跳机制，
    // 跳过帧范围与逐帧回调（导入无帧级数据）。显示走"阶段名 + 计时 + 批次 [i/N]"
    // （与导出 AS/LS/FBX 阶段同一模式——非 Solve 阶段无百分比，Percent 维持高水位）。
    if (State.bActive.load())
    {
        End(false);
    }

    State.AssetName = InTaskName;
    State.ProgressFilePath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/progress.json");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(State.ProgressFilePath), true);

    State.StartTime.store(FPlatformTime::Seconds());
    State.CurrentFrame.store(0);
    State.TotalFrames.store(0);
    State.FrameLowerBound.store(0);
    State.Stage.store(IsValidStage(InStage) ? InStage
        : static_cast<int32>(EMHSProgressStage::Import));
    State.PassIndex.store(0);
    State.PassCount.store(1);
    State.BroadcastCount.store(0);
    State.LastWrapTriggerFrame.store(-1);
    State.LastWrapTime.store(0.0);
    State.TaskCurrent.store(0);
    State.TaskTotal.store(0);       // 0 = 无任务进度（新段先重置，SetTaskContext 再开启）
    State.TaskConcurrency.store(0);
    State.TaskEtaSeconds.store(-1.0);
    {
        std::lock_guard<std::mutex> Lock(State.TaskActiveMutex);
        State.TaskActiveNames.Empty();
    }
    WeakPerf.Reset(); // 无 perf：End 的回调移除分支天然跳过

    State.bShouldStop.store(false);
    State.bActive.store(true);

    void* UeMainHwnd = nullptr;
    if (FSlateApplication::IsInitialized())
    {
        const TSharedPtr<SWindow> TopWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
        if (TopWindow.IsValid() && TopWindow->GetNativeWindow().IsValid())
        {
            UeMainHwnd = TopWindow->GetNativeWindow()->GetOSWindowHandle();
        }
    }
    State.MainWindowHwnd.store(UeMainHwnd);  // 心跳线程 poke 主窗口（防消息泵停滞）
    Window.Launch(InTaskName, UeMainHwnd);

    Worker = new FMHSProgressWorker(State, Window);
    WorkerThread = FRunnableThread::Create(Worker, TEXT("MHSProgressHeartbeat"), 0, TPri_Normal);

    UE_LOG(LogTemp, Log, TEXT("[MHS_LOG] 任务监控已启动: %s"), *InTaskName);
}

void FMHSProgressMonitor::SetStage(int32 NewStage)
{
    if (!State.bActive.load() || !IsValidStage(NewStage))
    {
        return;
    }
    State.Stage.store(NewStage);
    UE_LOG(LogTemp, Log, TEXT("[MHS_LOG] 进度阶段切换: %s"), GStageDisplayNames[NewStage]);
}

void FMHSProgressMonitor::SetBatchContext(int32 InIndex, int32 InTotal, double InEtaSeconds)
{
    // 批次上下文：纯 atomic 数值，任意时刻可安全调用（心跳线程只读）。
    // index/total <= 0 = 清空（无批次上下文）；index 从 1 计。
    State.BatchIndex.store(FMath::Max(0, InIndex));
    State.BatchTotal.store(FMath::Max(0, InTotal));
    State.BatchEtaSeconds.store(InEtaSeconds);
}

void FMHSProgressMonitor::SetTaskContext(int32 InTotal, int32 InConcurrency)
{
    // 任务进度上下文（批次导入段）：total<=0 清除任务进度；否则激活显示。
    // 纯 atomic + 字符串清空（加锁），任意时刻可安全调用。
    State.TaskTotal.store(FMath::Max(0, InTotal));
    State.TaskConcurrency.store(FMath::Max(0, InConcurrency));
    State.TaskCurrent.store(0);
    State.TaskEtaSeconds.store(-1.0);
    {
        std::lock_guard<std::mutex> Lock(State.TaskActiveMutex);
        State.TaskActiveNames.Empty();
    }
}

void FMHSProgressMonitor::UpdateTaskProgress(int32 InCurrent, double InEtaSeconds, const FString& InActiveNames)
{
    // 批次导入段内进度：C++ 并发调度每完成一个 take 调用（游戏线程）。
    // 心跳线程只读 atomic + 锁内读字符串，此处写安全。
    State.TaskCurrent.store(FMath::Max(0, InCurrent));
    State.TaskEtaSeconds.store(InEtaSeconds);
    {
        std::lock_guard<std::mutex> Lock(State.TaskActiveMutex);
        State.TaskActiveNames = InActiveNames;
    }
}

void FMHSProgressMonitor::End(bool bSuccess)
{
    if (!State.bActive.load())
    {
        return; // 幂等：未激活（如音频/视频非阻塞路径误调）直接返回
    }

    State.bActive.store(false);
    State.bShouldStop.store(true);

    // 顺序关键：先停心跳（最多等一个 500ms 周期），杜绝 PostMessage 与窗口销毁竞态
    if (WorkerThread)
    {
        WorkerThread->WaitForCompletion();
        delete WorkerThread;
        WorkerThread = nullptr;
    }

    // 停心跳后、销毁 Worker 前快照 Pass 耗时（终态行 2 Pass 摘要）
    TArray<double> PassDurations;
    double CurPassElapsed = 0.0;
    if (Worker)
    {
        PassDurations = Worker->GetCompletedPassDurations();
        CurPassElapsed = Worker->GetCurrentPassElapsed();
        delete Worker;
        Worker = nullptr;
    }

    // 终态展示（此刻无心跳线程竞争，可安全写显示数据）
    const double FinalPct = bSuccess ? 100.0 : ComputeWeightedPercent(State);
    FMHSWindowDisplayData Final;
    Final.bFinal = true;
    Final.bSuccess = bSuccess;
    Final.Percent = FinalPct;
    Final.Elapsed = FPlatformTime::Seconds() - State.StartTime.load();
    Final.Cur = State.CurrentFrame.load();
    Final.Total = State.TotalFrames.load();
    Final.Stage = State.Stage.load();
    Final.Pass = State.PassIndex.load();
    Final.PassCount = ClampPassCount(State.PassCount.load());
    Final.BatchIndex = State.BatchIndex.load();
    Final.BatchTotal = State.BatchTotal.load();
    Final.BatchEta = State.BatchEtaSeconds.load();
    Final.Label = State.AssetName;
    Final.PassSummary = BuildPassSummaryText(Final.PassCount, Final.Pass, PassDurations, CurPassElapsed, true);
    Window.PostFinal(Final); // 窗口收到 WM_CLOSE 后停留 0.8~1.2s 再销毁

    // 关窗（发 WM_CLOSE -> 定时器 -> DestroyWindow -> 消息循环退出 -> join）
    Window.Shutdown();

    // 移除逐帧回调（游戏线程）
    if (UMetaHumanPerformance* P = WeakPerf.Get())
    {
        P->OnFrameProcessed().Remove(FrameHandle);
    }
    WeakPerf.Reset();

    // 终态 json
    WriteFinalJson(bSuccess);

    UE_LOG(LogTemp, Log, TEXT("[MHS_LOG] 进度监控结束: %s（%s，耗时 %s）"),
        *State.AssetName, bSuccess ? TEXT("成功") : TEXT("失败"),
        *FormatDuration(Final.Elapsed));
}

void FMHSProgressMonitor::ForceStop()
{
    if (WorkerThread)
    {
        State.bActive.store(false);
        State.bShouldStop.store(true);
        WorkerThread->WaitForCompletion();
        delete WorkerThread;
        WorkerThread = nullptr;
    }
    if (Worker)
    {
        delete Worker;
        Worker = nullptr;
    }
    Window.Shutdown();

    if (UMetaHumanPerformance* P = WeakPerf.Get())
    {
        P->OnFrameProcessed().Remove(FrameHandle);
    }
    WeakPerf.Reset();
    State.bActive.store(false);
}

void FMHSProgressMonitor::WriteFinalJson(bool bSuccess)
{
    // 失败时保留实际进度（比写 0 诚实：跑到 60% 失败就记 60%）——按多 Pass 加权口径
    const double FinalPct = bSuccess ? 100.0 : ComputeWeightedPercent(State);

    const FString Json = FString::Printf(
        TEXT("{\n")
        TEXT("  \"asset_name\": \"%s\",\n")
        TEXT("  \"state\": \"%s\",\n")
        TEXT("  \"stage\": \"%s\",\n")
        TEXT("  \"pass\": %d,\n")
        TEXT("  \"pass_count\": %d,\n")
        TEXT("  \"current_frame\": %d,\n")
        TEXT("  \"total_frames\": %d,\n")
        TEXT("  \"percent\": %.1f,\n")
        TEXT("  \"elapsed_seconds\": %d,\n")
        TEXT("  \"eta_seconds\": 0,\n")
        TEXT("  \"batch_index\": %d,\n")
        TEXT("  \"batch_total\": %d,\n")
        TEXT("  \"batch_eta_seconds\": %.0f,\n")
        TEXT("  \"updated_at\": \"%s\"\n")
        TEXT("}"),
        *JsonEscape(State.AssetName),
        bSuccess ? TEXT("success") : TEXT("failed"),
        IsValidStage(State.Stage.load()) ? GStageJsonNames[State.Stage.load()] : TEXT("unknown"),
        FMath::Clamp(State.PassIndex.load(), 0, ClampPassCount(State.PassCount.load()) - 1) + 1,
        ClampPassCount(State.PassCount.load()),
        State.CurrentFrame.load(),
        State.TotalFrames.load(),
        FinalPct,
        static_cast<int32>(FPlatformTime::Seconds() - State.StartTime.load()),
        State.BatchIndex.load(),
        State.BatchTotal.load(),
        (State.BatchEtaSeconds.load() >= 0.0) ? State.BatchEtaSeconds.load() : -1.0,
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));

    const FString Tmp = State.ProgressFilePath + TEXT(".tmp");
    if (FFileHelper::SaveStringToFile(Json, *Tmp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        IFileManager::Get().Move(*State.ProgressFilePath, *Tmp, /*bReplace=*/true);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Python 入口（薄封装）
// ═══════════════════════════════════════════════════════════════════════

void UMetaHumanSolverProgressLibrary::BeginProgress(UMetaHumanPerformance* InPerformance, const FString& InAssetName, int32 InExpectedPasses)
{
    FMHSProgressMonitor::Get().Begin(InPerformance, InAssetName, InExpectedPasses);
}

void UMetaHumanSolverProgressLibrary::BeginTaskProgress(const FString& InTaskName, int32 InStage)
{
    FMHSProgressMonitor::Get().BeginTask(InTaskName, InStage);
}

void UMetaHumanSolverProgressLibrary::SetStage(int32 InStage)
{
    FMHSProgressMonitor::Get().SetStage(InStage);
}

void UMetaHumanSolverProgressLibrary::SetBatchContext(int32 InIndex, int32 InTotal, double InBatchEtaSeconds)
{
    FMHSProgressMonitor::Get().SetBatchContext(InIndex, InTotal, InBatchEtaSeconds);
}

void UMetaHumanSolverProgressLibrary::SetTaskContext(int32 InTotal, int32 InConcurrency)
{
    FMHSProgressMonitor::Get().SetTaskContext(InTotal, InConcurrency);
}

void UMetaHumanSolverProgressLibrary::EndProgress(bool bSuccess)
{
    FMHSProgressMonitor::Get().End(bSuccess);
}

#undef LOCTEXT_NAMESPACE
