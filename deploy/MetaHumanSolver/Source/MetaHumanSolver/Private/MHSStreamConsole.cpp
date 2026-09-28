#include "MHSStreamConsole.h"

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

namespace
{
    constexpr int32 CON_W = 100;   // 期望宽（列）
    constexpr int32 CON_H = 34;    // 期望高（行）
    // 实际生效尺寸：按控制台窗口可容纳大小调整（否则内容截断/出现滚动条/重绘跳动）
    int32 GRenderW = CON_W;
    int32 GRenderH = CON_H;

    // 显示宽度：中日韩字符占 2 列（conhost 对齐必须按显示宽度算，否则中文行会错位）
    int32 DisplayWidth(const FString& S)
    {
        int32 W = 0;
        for (const TCHAR& Ch : S)
        {
            W += (Ch >= 0x2E80) ? 2 : 1;
        }
        return W;
    }

    FString PadTo(const FString& S, int32 Width)
    {
        const int32 Pad = FMath::Max(0, Width - DisplayWidth(S));
        return S + FString::ChrN(Pad, TEXT(' '));
    }

    FString Sep(const FString& Title)
    {
        // 分隔线用 ASCII '-'：U+2500 在 CJK 字体下为全角，宽度不可控（曾导致整行错位）
        FString Line = TEXT("-- ") + Title + TEXT(" ");
        const int32 Need = FMath::Max(0, GRenderW - 3 - DisplayWidth(Line));
        return Line + FString::ChrN(Need, TEXT('-'));
    }

    FString Bar(double Percent)
    {
        const int32 Filled = FMath::Clamp(FMath::RoundToInt(Percent / 5.0), 0, 20);
        return FString::ChrN(Filled, TEXT('#')) + FString::ChrN(20 - Filled, TEXT('-'));
    }

    // "2h38m" / "7m20s"
    FString Dur(double Seconds)
    {
        if (Seconds < 0) { return TEXT("—"); }
        const int32 S = FMath::RoundToInt(Seconds);
        const int32 H = S / 3600, M = (S % 3600) / 60;
        return H > 0 ? FString::Printf(TEXT("%dh%02dm"), H, M)
                     : FString::Printf(TEXT("%dm%02ds"), M, S % 60);
    }
}

MHSStreamConsole& MHSStreamConsole::Get()
{
    static MHSStreamConsole Inst;
    return Inst;
}

void MHSStreamConsole::Launch(const FString& InInbox, const FString& InOutputDir)
{
    InboxPath = InInbox;
    OutputPath = InOutputDir;
    StatePath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/stream_state.json");
    ProgressPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/progress.json");

    if (bRunning.load())
    {
        return;   // 幂等：已在运行
    }

    // 控制台归属：本控制台先创建（[启动监听] 时），之后单任务窗 AllocConsole 会失败
    // → 其 bOwnedConsole=false → 段结束不会 FreeConsole 摘掉全局控制台
    if (!::GetConsoleWindow())
    {
        ::AllocConsole();
    }
    ::SetConsoleTitleW(L"Stream 全局控制台");

    void* HOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (HOut && HOut != INVALID_HANDLE_VALUE)
    {
        // 尺寸设置的正确顺序（否则出现滚动条 / 内容被截断 / 每次重绘视觉跳动）：
        //   ① 取窗口可容纳的最大尺寸，取 min(期望, 可用)
        //   ② 先把 buffer 设为比窗口大（避免缩小窗口时被拒）
        //   ③ 再设窗口矩形
        //   ④ 最后把 buffer 收到与窗口一致 → 无滚动条
        const COORD Largest = ::GetLargestConsoleWindowSize(HOut);
        const SHORT W = (SHORT)FMath::Min<int32>(CON_W, FMath::Max<int32>(40, Largest.X));
        const SHORT H = (SHORT)FMath::Min<int32>(CON_H, FMath::Max<int32>(15, Largest.Y));
        ::SetConsoleScreenBufferSize(HOut, { W, (SHORT)(H + 1) });
        SMALL_RECT R = { 0, 0, (SHORT)(W - 1), (SHORT)(H - 1) };
        ::SetConsoleWindowInfo(HOut, true, &R);
        ::SetConsoleScreenBufferSize(HOut, { W, H });
        GRenderW = W;
        GRenderH = H;
        // 关闭快速编辑（避免鼠标点击阻塞渲染线程）
        DWORD Mode = 0;
        if (::GetConsoleMode(HOut, &Mode))
        {
            ::SetConsoleMode(HOut, (Mode | ENABLE_PROCESSED_OUTPUT) & ~ENABLE_QUICK_EDIT_MODE);
        }
    }
    if (HWND H = ::GetConsoleWindow())
    {
        ::SetWindowPos(H, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        // 隐藏光标：否则全屏重绘时光标持续闪烁（观感像"被反复删除"）
        CONSOLE_CURSOR_INFO Ci;
        Ci.dwSize = 1;
        Ci.bVisible = false;
        ::SetConsoleCursorInfo(HOut, &Ci);
    }

    bStop.store(false);
    bRunning.store(true);
    Thread = std::thread([this]() { ThreadMain(); });
}

void MHSStreamConsole::Shutdown()
{
    if (!bRunning.load())
    {
        return;
    }
    bStop.store(true);
    if (Thread.joinable())
    {
        Thread.join();
    }
    bRunning.store(false);
}

void MHSStreamConsole::ThreadMain()
{
    while (!bStop.load())
    {
        RenderOnce();
        // 1s 刷新（分段 sleep 以便及时响应停止；每段顺带轮询按键）
        for (int32 i = 0; i < 10 && !bStop.load(); ++i)
        {
            PollKeys();
            FPlatformProcess::Sleep(0.1f);
        }
    }
}

void MHSStreamConsole::PollKeys()
{
    // 按键轮询（按 100ms 粒度）：仅当控制台窗口是前台窗口时响应——
    // 否则在编辑器里打字的 T/O 会被误捕获。
    // 用 GetAsyncKeyState 的最低位（"上次调用后按下过"），保证快按不丢。
    HWND Con = ::GetConsoleWindow();
    if (!Con || ::GetForegroundWindow() != Con)
    {
        return;
    }
    if (::GetAsyncKeyState('T') & 0x0001)
    {
        bTopmost = !bTopmost;
        ::SetWindowPos(Con, bTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                       0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    if (::GetAsyncKeyState('O') & 0x0001)
    {
        // 打开 FBX 输出目录：优先 job_stream.json 的 fbx_output_dir（配置真实来源），
        // 回落 Launch 时传入的 OutputPath，再回落项目 Saved。
        FString OutDir;
        const FString JobPath = FPaths::ProjectSavedDir() / TEXT("Config/MetaHumanSolver/job_stream.json");
        FString JsonStr;
        if (FFileHelper::LoadFileToString(JsonStr, *JobPath))
        {
            TSharedPtr<FJsonObject> Root;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
            if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
            {
                Root->TryGetStringField(TEXT("fbx_output_dir"), OutDir);
            }
        }
        if (OutDir.IsEmpty())
        {
            OutDir = OutputPath;
        }
        if (OutDir.IsEmpty())
        {
            OutDir = FPaths::ProjectSavedDir();
        }
        FPlatformProcess::ExploreFolder(*OutDir);
    }
}

bool MHSStreamConsole::LoadStateJson(TSharedPtr<FJsonObject>& OutRoot) const
{
    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *StatePath))
    {
        return false;
    }
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    return FJsonSerializer::Deserialize(Reader, OutRoot) && OutRoot.IsValid();
}

bool MHSStreamConsole::LoadProgressJson(TSharedPtr<FJsonObject>& OutRoot) const
{
    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *ProgressPath))
    {
        return false;
    }
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    return FJsonSerializer::Deserialize(Reader, OutRoot) && OutRoot.IsValid();
}

void MHSStreamConsole::RenderOnce()
{
    struct FLine { FString Text; WORD Attr; };
    TArray<FLine> Lines;
    auto Add = [&Lines](const FString& T, WORD A)
    {
        Lines.Add({ PadTo(T, GRenderW - 1), A });
    };
    const WORD DIM = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    const WORD BLUE = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    const WORD GREEN = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    const WORD AMBER = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    const WORD RED = FOREGROUND_RED | FOREGROUND_INTENSITY;
    const WORD WHITE = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;

    const FDateTime NowDt = FDateTime::Now();
    const FString Now = NowDt.ToString(TEXT("%H:%M:%S"));

    // 按控制台实际 buffer 尺寸渲染（否则内容截断 / 出现滚动条 / 重绘跳动）
    if (void* HOutNow = ::GetStdHandle(STD_OUTPUT_HANDLE))
    {
        CONSOLE_SCREEN_BUFFER_INFO Info;
        if (HOutNow != INVALID_HANDLE_VALUE
            && ::GetConsoleScreenBufferInfo(HOutNow, &Info))
        {
            GRenderW = FMath::Max<int32>(40, Info.dwSize.X);
            GRenderH = FMath::Max<int32>(15, Info.dwSize.Y);
        }
    }

    Add(FString::Printf(TEXT("$ stream --watch %s"), *InboxPath), WHITE);
    Add(TEXT("  身份=当前绑定 | 加载中=已发现(等待就绪/身份绑定) | 处理中=正在导入或解算 | 已完成=FBX 已交付"),
        DIM);
    Add(TEXT(""), DIM);

    // ── 身份带 ──
    Add(Sep(TEXT("身份")), DIM);
    // ── 先读两个数据源（供新鲜度与常驻进度行使用）──
    TSharedPtr<FJsonObject> St;
    const bool bHasState = LoadStateJson(St);
    TSharedPtr<FJsonObject> Pg;
    const bool bHasProgress = LoadProgressJson(Pg);

    // 新鲜度：取两个文件里较新的 updated_at。
    // 关键：解算 34 分钟阻塞期间 stream_state 无人写入（executor 阻塞中），
    // 但 progress.json 由 C++ 心跳 500ms 持续更新 → 不能误报"陈旧/未监听"。
    FString DataStamp = Now;
    FString FreshTag;
    FString PStateStr;
    bool bProgressLive = false;   // 底层是否真的在跑（progress 心跳新鲜）
    {
        auto ParseStamp = [&NowDt](const TSharedPtr<FJsonObject>& O, double& OutAge) -> bool
        {
            FString U;
            if (!O.IsValid() || !O->TryGetStringField(TEXT("updated_at"), U) || U.Len() < 19)
            {
                return false;
            }
            FDateTime Stamp;
            if (!FDateTime::Parse(U, Stamp))
            {
                return false;
            }
            OutAge = (NowDt - Stamp).GetTotalSeconds();
            return true;
        };
        double AgeState = -1.0, AgeProg = -1.0;
        bool bHasS = ParseStamp(St, AgeState);
        bool bHasP = ParseStamp(Pg, AgeProg);
        double Best = -1.0;
        if (bHasS) { Best = AgeState; }
        if (bHasP && (Best < 0.0 || AgeProg < Best)) { Best = AgeProg; }
        if (bHasProgress)
        {
            Pg->TryGetStringField(TEXT("state"), PStateStr);
            // progress 心跳 500ms：10 秒内更新过即认为底层活跃（解算/导入/导出中）
            bProgressLive = (AgeProg >= 0.0 && AgeProg < 10.0)
                && (PStateStr == TEXT("solving") || PStateStr == TEXT("import")
                    || PStateStr == TEXT("export"));
        }
        if (Best < 0.0)
        {
            FreshTag = TEXT("   (无数据)");
        }
        else if (Best >= 60.0 && !bProgressLive)
        {
            // 未监听时不必用"陈旧"字样质问（那是用户主动停的，不是故障）
            FString StateHint;
            if (St.IsValid()) { St->TryGetStringField(TEXT("state"), StateHint); }
            const bool bIdle = (StateHint != TEXT("running"));
            FreshTag = bIdle
                ? FString::Printf(TEXT("   (未监听 · 显示上次会话数据，%.0f 分钟前)"), Best / 60.0)
                : FString::Printf(TEXT("   [数据陈旧 %.0f 分钟前——监听可能已中断]"), Best / 60.0);
        }
        else if (bProgressLive && Best >= 60.0)
        {
            FreshTag = FString::Printf(TEXT("   (列表未刷新，底层运行中)"));
        }
        else
        {
            FreshTag = FString::Printf(TEXT("   (%.0f 秒前)"), FMath::Max(0.0, Best));
        }
        // 时间戳显示：用较新那个数据文件的时刻
        FString Newer;
        if (bHasP && (!bHasS || AgeProg <= AgeState))
        {
            Pg->TryGetStringField(TEXT("updated_at"), Newer);
        }
        else if (bHasS)
        {
            St->TryGetStringField(TEXT("updated_at"), Newer);
        }
        if (Newer.Len() >= 19) { DataStamp = Newer.RightChop(11); }
    }

    if (bHasState)
    {
        const TSharedPtr<FJsonObject>* Ident = nullptr;
        if (St->TryGetObjectField(TEXT("identity"), Ident) && Ident && (*Ident).IsValid())
        {
            FString Group, Asset, State, Detail, Source, Suggested;
            (*Ident)->TryGetStringField(TEXT("group"), Group);
            (*Ident)->TryGetStringField(TEXT("asset"), Asset);
            (*Ident)->TryGetStringField(TEXT("state"), State);
            (*Ident)->TryGetStringField(TEXT("detail"), Detail);
            (*Ident)->TryGetStringField(TEXT("source"), Source);           // 最近 ROM 素材名
            (*Ident)->TryGetStringField(TEXT("suggested_group"), Suggested); // 从 ROM 名识别的组
            const bool bBound = (State == TEXT("已绑定"));
            // 尾列：已绑定展示"状态 + ROM 来源（+建议组）"；否则"状态 · 明细"。
            // （修复：已绑定态 detail 就是 asset，直接打印会同一路径重复两遍）
            FString Tail;
            if (bBound)
            {
                Tail = TEXT("已绑定");
                if (!Source.IsEmpty())
                {
                    Tail += FString::Printf(TEXT(" · 来源 %s"), *Source);
                }
                if (!Suggested.IsEmpty() && Suggested != Group)
                {
                    Tail += FString::Printf(TEXT("（ROM 建议 %s/）"), *Suggested);
                }
            }
            else
            {
                Tail = Detail.IsEmpty()
                    ? State
                    : ((Detail == Asset) ? State : FString::Printf(TEXT("%s · %s"), *State, *Detail));
            }
            Add(FString::Printf(TEXT("[%s] [i] 身份  %s -> %s   | %s"),
                *DataStamp, Group.IsEmpty() ? TEXT("(根)") : *Group,
                Asset.IsEmpty() ? TEXT("<未绑定>") : *Asset, *Tail),
                bBound ? GREEN : AMBER);
            // 身份带行也带一次新鲜度提示（只在此行显示，避免每行冗长）
        }
        // 状态 + 追平
        int32 Queued = 0, Delivered = 0, Failed = 0;
        St->TryGetNumberField(TEXT("queued"), Queued);
        St->TryGetNumberField(TEXT("delivered"), Delivered);
        St->TryGetNumberField(TEXT("failed"), Failed);
        FString StateStr; St->TryGetStringField(TEXT("state"), StateStr);
        // 监听态判定：状态文件 running，或 progress 心跳新鲜（底层确在跑）——
        // 状态文件在任务开始时可能尚未刷新，单看它会把"导入中"误报成"未监听"
        const bool bRun = (StateStr == TEXT("running")) || bProgressLive;
        Add(FString::Printf(TEXT("[%s] %s   待处理 %d | 已交付 %d | 失败 %d%s"),
            *DataStamp, bRun ? TEXT("[*] 监听中") : TEXT("[-] 未监听"),
            Queued, Delivered, Failed, *FreshTag),
            FreshTag.Contains(TEXT("陈旧")) ? AMBER : (bRun ? BLUE : DIM));
        double Eta = -1.0;
        St->TryGetNumberField(TEXT("eta_seconds"), Eta);
        if (Queued > 0 && Eta >= 0)
        {
            const FDateTime Done = FDateTime::Now() + FTimespan::FromSeconds(Eta);
            Add(FString::Printf(TEXT("[%s] 预计追平 %02d:%02d (%s)"),
                *DataStamp, Done.GetHour(), Done.GetMinute(), *Dur(Eta)), BLUE);
        }
        else if (Queued > 0)
        {
            Add(FString::Printf(TEXT("[%s] 待处理 %d 条（暂无耗时历史，无法预估追平）"), *DataStamp, Queued), DIM);
        }
        else
        {
            Add(FString::Printf(TEXT("[%s] 没有待处理素材"), *DataStamp), DIM);
        }

        // ── 当前进度：仅在底层活跃（progress 心跳 10s 内）时渲染 ──
        // 修复：曾只判 bHasProgress——上次会话的终态导入（99%/已进行 2m21s）会被
        // 当成"进行中"展示，用户看到的是 json 残留而非实时检测。
        // 空闲时整个区块不渲染（信息密度优先）。
        if (bHasProgress && bProgressLive)
        {
            FString Asset; double Pct = 0, ElapsedP = 0, PEta = -1;
            int32 Pass = 0, PassCount = 1, Cur = 0, Total = 0;
            Pg->TryGetStringField(TEXT("asset_name"), Asset);
            Pg->TryGetNumberField(TEXT("percent"), Pct);
            Pg->TryGetNumberField(TEXT("elapsed_seconds"), ElapsedP);
            Pg->TryGetNumberField(TEXT("eta_seconds"), PEta);
            Pg->TryGetNumberField(TEXT("pass"), Pass);
            Pg->TryGetNumberField(TEXT("pass_count"), PassCount);
            Pg->TryGetNumberField(TEXT("current_frame"), Cur);
            Pg->TryGetNumberField(TEXT("total_frames"), Total);
            const TCHAR* StageCn = (PStateStr == TEXT("solving")) ? TEXT("深度解算")
                : (PStateStr == TEXT("import")) ? TEXT("导入转换")
                : (PStateStr == TEXT("export")) ? TEXT("导出") : TEXT("空闲");
            Add(TEXT(""), DIM);
            Add(Sep(TEXT("当前进度（每 1 秒刷新）")), DIM);
            if (PStateStr == TEXT("solving"))
            {
                Add(FString::Printf(TEXT("  %s  %s  [%s] %.0f%%  Pass %d/%d  %d/%d 帧"),
                    *Asset, StageCn, *Bar(Pct), Pct, Pass, FMath::Max(1, PassCount), Cur, Total),
                    BLUE);
                Add(FString::Printf(TEXT("  已跑 %s | 剩余约 %s（解算期间面板冻结，此窗口照常刷新）"),
                    *Dur(ElapsedP), *Dur(PEta)), DIM);
            }
            else if (PStateStr == TEXT("import"))
            {
                // 导入阶段引擎不提供帧进度 → 按素材体积估算时长（执行器注入
                // import_estimate_seconds，吞吐 ~1.5MB/s 实测标定）。
                // 修复：曾用固定 30s 基准 → 大素材进度条停在 99% 数分钟。
                double EstTotal = 30.0;
                St->TryGetNumberField(TEXT("import_estimate_seconds"), EstTotal);
                EstTotal = FMath::Max(10.0, EstTotal);
                if (ElapsedP <= EstTotal * 1.2)
                {
                    const double EstPct = FMath::Clamp(ElapsedP / EstTotal * 100.0, 0.0, 95.0);
                    Add(FString::Printf(TEXT("  %s  %s  [%s] %.0f%%（预计）  已进行 %s / 约 %s"),
                        *Asset, StageCn, *Bar(EstPct), EstPct, *Dur(ElapsedP), *Dur(EstTotal)), BLUE);
                }
                else
                {
                    // 超出预估（大素材）：撤掉假进度条，诚实显示"以实际完成为准"
                    Add(FString::Printf(TEXT("  %s  %s  已进行 %s（超出预估 %s——大素材以实际完成为准）"),
                        *Asset, StageCn, *Dur(ElapsedP), *Dur(EstTotal)), BLUE);
                }
            }
            else if (PStateStr == TEXT("export"))
            {
                Add(FString::Printf(TEXT("  %s  %s  已进行 %s"), *Asset, StageCn, *Dur(ElapsedP)), BLUE);
            }
            else
            {
                // 终态残留（上轮任务的 success/failed）：不展示其资产名与"空闲"，避免误导
                Add(TEXT("  （无进行中的任务——解算开始后此处每秒刷新）"), DIM);
            }
        }
        // 未活跃（无进度/心跳陈旧）→ 整个"当前进度"区块不渲染（空闲不占版面）

        // ── 状态区块（空区块整体隐藏——信息密度优先，空闲时只有一行）──
        const TSharedPtr<FJsonObject>* Detail = nullptr;
        if (St->TryGetObjectField(TEXT("tasks_detail"), Detail) && Detail && (*Detail).IsValid())
        {
            enum { SecLoading = 0, SecFailed = 1, SecActive = 2, SecDone = 3, SecNum = 4 };
            TArray<FString> SecLines[SecNum];
            TArray<WORD> SecAttr[SecNum];
            auto Put = [&](int32 Idx, const FString& T, WORD A)
            {
                SecLines[Idx].Add(PadTo(T, GRenderW - 1));
                SecAttr[Idx].Add(A);
            };
            auto EmitSection = [&](const TCHAR* Title, int32 Idx)
            {
                if (SecLines[Idx].Num() == 0) { return; }
                Add(TEXT(""), DIM);
                Add(Sep(Title), DIM);
                for (int32 i = 0; i < SecLines[Idx].Num(); ++i)
                {
                    Add(SecLines[Idx][i], SecAttr[Idx][i]);
                }
            };

            // 加载中：等待就绪 / 待解算 / 待绑定 / 盘上未接管
            const TArray<TSharedPtr<FJsonValue>>* Loading = nullptr;
            if ((*Detail)->TryGetArrayField(TEXT("loading"), Loading) && Loading)
            {
                int32 Count = 0;
                for (const auto& V : *Loading)
                {
                    if (Count++ >= 6) { break; }
                    const TSharedPtr<FJsonObject> O = V->AsObject();
                    FString Key, Note;
                    bool bReady = false;
                    if (O.IsValid())
                    {
                        O->TryGetStringField(TEXT("key"), Key);
                        O->TryGetStringField(TEXT("note"), Note);
                        O->TryGetBoolField(TEXT("ready"), bReady);
                    }
                    Put(SecLoading, FString::Printf(TEXT("  %s   %s"), *Key, *Note),
                        bReady ? DIM : AMBER);
                }
                if (Loading->Num() > 6)
                {
                    Put(SecLoading, FString::Printf(TEXT("  ... 另有 %d 条"), Loading->Num() - 6), DIM);
                }
            }
            // 磁盘实时视图：已到达但队列尚未接管的素材（解算阻塞期执行器失明，
            // 但控制台直接看盘 → 用户"拖进去"能立刻看到反馈，而不是以为没反应）
            {
                TSet<FString> Known;
                auto CollectKeys = [&Known](const TArray<TSharedPtr<FJsonValue>>* Arr)
                {
                    if (!Arr) { return; }
                    for (const auto& V : *Arr)
                    {
                        const TSharedPtr<FJsonObject> O = V->AsObject();
                        FString K;
                        if (O.IsValid() && O->TryGetStringField(TEXT("key"), K) && !K.IsEmpty())
                        {
                            Known.Add(K);
                        }
                    }
                };
                CollectKeys(Loading);
                const TArray<TSharedPtr<FJsonValue>>* ActArr = nullptr;
                if ((*Detail)->TryGetArrayField(TEXT("active"), ActArr)) { CollectKeys(ActArr); }
                const TArray<TSharedPtr<FJsonValue>>* DoneArr = nullptr;
                if ((*Detail)->TryGetArrayField(TEXT("done"), DoneArr)) { CollectKeys(DoneArr); }
                // failed/imported 同理：素材目录仍在热文件夹，不收进 known 会被
                // [盘] 行永远误标"已到达 · 未监听"（e011 / ID 两类误报的教训）
                const TArray<TSharedPtr<FJsonValue>>* FailArr = nullptr;
                if ((*Detail)->TryGetArrayField(TEXT("failed"), FailArr)) { CollectKeys(FailArr); }
                const TArray<TSharedPtr<FJsonValue>>* ImportedArr = nullptr;
                if ((*Detail)->TryGetArrayField(TEXT("imported"), ImportedArr)) { CollectKeys(ImportedArr); }

                auto ListArrived = [&Put, &Known, bRun](const FString& Dir, const FString& KeyPrefix)
                {
                    TArray<FString> Names;
                    IFileManager::Get().FindFiles(Names, *(Dir / TEXT("*")), /*Files=*/false, /*Dirs=*/true);
                    for (const FString& N : Names)
                    {
                        const FString Key = KeyPrefix + N;
                        if (Known.Contains(Key)) { continue; }
                        // 仅列出 take 目录（含 take.json）——忽略杂项目录
                        if (!IFileManager::Get().FileExists(*(Dir / N / TEXT("take.json")))) { continue; }
                        Put(SecLoading, bRun
                            ? FString::Printf(TEXT("  [盘] %s   已到达 · 等待接管（当前任务完成后自动处理）"), *Key)
                            : FString::Printf(TEXT("  [盘] %s   已到达 · 未监听（点 [启动监听] 后处理）"), *Key),
                            AMBER);
                    }
                };
                // 动态枚举加载路径下所有组目录——曾硬编码只扫 pm/_id，
                // am（及未来任何组名）的素材永远不会出现在加载区（纯显示层遗漏）
                TArray<FString> TopDirs;
                IFileManager::Get().FindFiles(TopDirs, *(InboxPath / TEXT("*")), /*Files=*/false, /*Dirs=*/true);
                for (const FString& G : TopDirs)
                {
                    if (G == TEXT("_id"))
                    {
                        ListArrived(InboxPath / G, TEXT("_id/"));
                    }
                    else if (IFileManager::Get().FileExists(*(InboxPath / G / TEXT("take.json"))))
                    {
                        // 根目录直放 take（无组）
                        if (!Known.Contains(G))
                        {
                            Put(SecLoading, bRun
                                ? FString::Printf(TEXT("  [盘] %s   已到达 · 等待接管（当前任务完成后自动处理）"), *G)
                                : FString::Printf(TEXT("  [盘] %s   已到达 · 未监听（点 [启动监听] 后处理）"), *G),
                                AMBER);
                        }
                    }
                    else
                    {
                        ListArrived(InboxPath / G, G + TEXT("/"));
                    }
                }
            }
            // 已导入 ID 素材（ROM）——note 由状态层按"该身份是否已绑定"给出
            const TArray<TSharedPtr<FJsonValue>>* ImpArr = nullptr;
            if ((*Detail)->TryGetArrayField(TEXT("imported"), ImpArr) && ImpArr)
            {
                for (int32 i = 0; i < ImpArr->Num() && i < 3; ++i)
                {
                    const TSharedPtr<FJsonObject> O = (*ImpArr)[i]->AsObject();
                    FString K, Note;
                    if (O.IsValid())
                    {
                        O->TryGetStringField(TEXT("key"), K);
                        O->TryGetStringField(TEXT("note"), Note);
                    }
                    Put(SecLoading, FString::Printf(TEXT("  [ID] %s   %s"), *K,
                        Note.IsEmpty() ? TEXT("已导入 · 等待制作/绑定身份") : *Note), WHITE);
                }
            }

            // 已失败（含原因）
            const TArray<TSharedPtr<FJsonValue>>* FailedArr = nullptr;
            if ((*Detail)->TryGetArrayField(TEXT("failed"), FailedArr) && FailedArr)
            {
                int32 FCount = 0;
                for (const auto& V : *FailedArr)
                {
                    if (FCount++ >= 6) { break; }
                    const TSharedPtr<FJsonObject> O = V->AsObject();
                    FString K, Err;
                    if (O.IsValid())
                    {
                        O->TryGetStringField(TEXT("key"), K);
                        O->TryGetStringField(TEXT("error"), Err);
                    }
                    Put(SecFailed, FString::Printf(TEXT("  [败] %s   %s"), *K, *Err), RED);
                }
                if (FailedArr->Num() > 6)
                {
                    Put(SecFailed, FString::Printf(TEXT("  ... 另有 %d 条"), FailedArr->Num() - 6), DIM);
                }
            }

            // 处理中（正在执行的任务）——只给"哪个任务、什么阶段"，帧级进度看顶部
            const TArray<TSharedPtr<FJsonValue>>* Active = nullptr;
            if ((*Detail)->TryGetArrayField(TEXT("active"), Active) && Active)
            {
                int32 ACount = 0;
                for (const auto& V : *Active)
                {
                    if (ACount++ >= 2) { break; }
                    const TSharedPtr<FJsonObject> O = V->AsObject();
                    FString Key, Status, Kind;
                    if (O.IsValid())
                    {
                        O->TryGetStringField(TEXT("key"), Key);
                        O->TryGetStringField(TEXT("status"), Status);
                        O->TryGetStringField(TEXT("kind"), Kind);
                    }
                    const TCHAR* TypeTag = (Kind == TEXT("id")) ? TEXT("[ID]") : TEXT("[表演]");
                    const TCHAR* Cn = (Status == TEXT("importing")) ? TEXT("导入素材中")
                        : (Status == TEXT("solving")) ? TEXT("深度解算中")
                        : (Status == TEXT("exporting")) ? TEXT("导出中") : TEXT("处理中");
                    Put(SecActive, FString::Printf(TEXT("  %s %s   %s"), TypeTag, *Key, Cn), BLUE);
                }
            }

            // 已完成（仅表演交付，最新在上）
            const TArray<TSharedPtr<FJsonValue>>* Done = nullptr;
            if ((*Detail)->TryGetArrayField(TEXT("done"), Done) && Done)
            {
                int32 DCount = 0;
                for (int32 i = Done->Num() - 1; i >= 0 && DCount < 8; --i, ++DCount)
                {
                    const TSharedPtr<FJsonObject> O = (*Done)[i]->AsObject();
                    FString Key;
                    double D = 0;
                    if (O.IsValid())
                    {
                        O->TryGetStringField(TEXT("key"), Key);
                        O->TryGetNumberField(TEXT("duration"), D);
                    }
                    Put(SecDone, FString::Printf(TEXT("  [v] %s   %s"), *Key, *Dur(D)), GREEN);
                }
                if (Done->Num() > 8)
                {
                    Put(SecDone, FString::Printf(TEXT("  ... 另有 %d 条"), Done->Num() - 8), DIM);
                }
            }

            // 只渲染有内容的区块；全空 = 一行空闲（不再出现多个"（无）"占位）
            const bool bAnyContent = SecLines[SecLoading].Num() + SecLines[SecFailed].Num()
                + SecLines[SecActive].Num() + SecLines[SecDone].Num() > 0;
            if (bAnyContent)
            {
                EmitSection(TEXT("加载中（等待就绪 / 待解算 / 待绑定）"), SecLoading);
                EmitSection(TEXT("处理中（正在执行的任务）"), SecActive);
                EmitSection(TEXT("已失败（把素材放回热文件夹后自动重新排队）"), SecFailed);
                EmitSection(TEXT("已完成（仅表演交付）"), SecDone);
            }
            else
            {
                Add(TEXT(""), DIM);
                Add(Sep(TEXT("状态")), DIM);
                Add(TEXT("  （空闲——素材拷入热文件夹后自动开始处理）"), DIM);
            }
        }
    }
    else
    {
        Add(TEXT("（等待 stream_state.json —— 点 [启动监听] 后生成）"), AMBER);
    }

    Add(TEXT(""), DIM);
    Add(FString::ChrN(GRenderW - 1, TEXT('-')), DIM);
    Add(TEXT("T 置顶切换   O 打开输出目录   （关闭窗口=隐藏，不终止流程）"), DIM);

    // ── 整屏写入（CHAR_INFO 矩形，无 cls → 无闪烁）──
    // 内容未变化则跳过写入：1s 轮询下绝大多数帧内容相同，跳过可彻底消除重绘抖动
    FString CurFrame;
    for (const FLine& L : Lines) { CurFrame += L.Text; CurFrame += TEXT("\n"); }
    if (CurFrame == LastFrame)
    {
        return;
    }
    LastFrame = CurFrame;

    void* HOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!HOut || HOut == INVALID_HANDLE_VALUE) { return; }

    TArray<CHAR_INFO> Buf;
    Buf.SetNum(GRenderW * GRenderH);
    for (auto& C : Buf)
    {
        C.Char.UnicodeChar = TEXT(' ');
        C.Attributes = DIM;
    }
    for (int32 y = 0; y < Lines.Num() && y < GRenderH; ++y)
    {
        const FString& T = Lines[y].Text;
        int32 x = 0;
        for (const TCHAR& Ch : T)
        {
            if (x >= GRenderW)
            {
                break;
            }
            const bool bWide = (Ch >= 0x2E80);   // 全角字符在控制台占 2 个 cell
            Buf[y * GRenderW + x].Char.UnicodeChar = Ch;
            Buf[y * GRenderW + x].Attributes = Lines[y].Attr;
            if (bWide && x + 1 < GRenderW)
            {
                // 第二格留空（编码 0）：否则后续字符会被渲染挤位/重叠
                Buf[y * GRenderW + x + 1].Char.UnicodeChar = 0;
                Buf[y * GRenderW + x + 1].Attributes = Lines[y].Attr;
                x += 2;
            }
            else
            {
                x += 1;
            }
        }
    }
    const COORD Size = { (SHORT)GRenderW, (SHORT)GRenderH };
    const COORD Zero = { 0, 0 };
    SMALL_RECT Region = { 0, 0, (SHORT)(GRenderW - 1), (SHORT)(GRenderH - 1) };
    ::WriteConsoleOutputW(HOut, Buf.GetData(), Size, Zero, &Region);
}

#endif
