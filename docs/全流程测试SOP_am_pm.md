# 全流程测试 SOP · am + pm 双组（从制作身份开始）

> 目标：一次跑通**两个组（am / pm）各带独立身份**的完整流式工作流——
> ROM 导入 → 人工制作身份 → 绑定 → 素材入队 → 解算 → FBX 落盘。
> 环境已备好（accept4），本 SOP 可直接照做。

---

## 0. 环境（accept5，2026-09-22 全新重开）

```
E:\Work\Mobu_maya\MetaSol\spike\
  accept5_source\                  ← 素材源（不要动，测试时从这里"复制"）
    _id\0818_face_rom_am_001_11                        ← am 的 ROM（41MB，已导入过→秒完成）
    am\event__emphasis_peak_lock__lock_emp_003_001     （185MB）
    am\prosody__intonation__final_confirm_rise__int_001_b_001（201MB）
    pm\context_answer__highamp__hap__e001_answer_001   （241MB）
  accept5_inbox\                   ← 热文件夹（加载路径，测试时把素材拷进来）
    am\  pm\  _id\                  （空）
  accept5_out\                     ← FBX 输出目录（空）
```

> accept5 四条素材**全部未参与过任何测试**（预检已验证：ROM=id、am×2=perf/am、pm×1=perf/pm，幂等复扫 0 条）。
> 面板配置已直写到 `stream_panel.json`（打开即回填 accept5 路径），am 绑定已保留（身份在 `/Game/CaptureManager/ID4/0818_face_rom_am_001_11`，无需重做身份）。

**pm 身份**已有现成资产：`/Game/ID/20260907_PM_ROM02_13`（上次做的）
**am 身份**本次从零制作（这就是"从制作 ID 开始"的部分）

---

## 1. 一次性准备

### 1a. 清空旧运行状态（获得全新起点）

**在 PowerShell 里粘贴执行**（我不便代执行——删除类命令需要弹窗授权）：

```powershell
$cfg="F:\CFH\CFH\20260709\MH_H\Saved\Config\MetaHumanSolver"
foreach ($f in @("queue_state.json","stream_state.json","stream_bindings.json","progress.json","job_stream.json")) {
  if (Test-Path "$cfg\$f") { Remove-Item "$cfg\$f" -Force; "REMOVED: $f" }
}
```

> 不清也没关系：旧的 pm/失败任务会出现在控制台——但那会让"全新"测试的观察面变脏。
> （旧 `stream_panel.json` 保留——面板配置会回填，改一下路径即可。）

### 1b. 面板配置（Stream Tab 配置区）

| 字段 | 填什么 |
|---|---|
| 加载路径（热文件夹） | `E:\Work\Mobu_maya\MetaSol\spike\accept4_inbox` |
| 身份导入 | `/Game/CaptureManager/ID4` ← **换成新路径**，避免与历史 ROM 资产撞名 |
| 资产目录 | `/Game/ID` |
| 身份资产 | **留空**（每次绑定前点[检测]填入，再按需改） |
| 素材导入 | `/Game/CaptureManager/Auto4` ← 同上，新路径 |
| 导出路径 | `E:\Work\Mobu_maya\MetaSol\spike\accept4_out` |

→ 点 **[保存为默认]**（下次打开自动回填）

### 1c. 启动监听

点 **[启动监听]** → 控制台出现 → 应显示 `[-] 未监听`→`[*] 监听中`、身份带、三列区块。

> 控制台快捷键（**本轮刚实现**）：
> `T` = 置顶切换（需控制台窗口在前台时按）
> `O` = 打开导出目录（读 job 的 fbx_output_dir）
> 关闭窗口 = 隐藏（不终止流程）

---

## 2. 阶段 A：am（身份已成品 → 直接绑定 → 3 条素材）

```
① 绑定 am（身份已在 /Game/CaptureManager/ID4/0818_face_rom_am_001_11，
   面板"身份资产"已预填该路径——直接点 [确认绑定]）：
     绑定目标 = am  → [确认绑定]
   日志："身份绑定：/Game/CaptureManager/ID4/0818_face_rom_am_001_11 -> 分组 'am'"

② 拷 am 第一条 → accept6_inbox\am\
   → ≤15 秒：加载中"已加载 · 待解算" → 处理中"导入素材中" → "深度解算中"
③ 解算进行中再拷第 2、3 条
   → 加载中区显示 [盘]"已到达 · 等待接管"（★ am 也能显示，此前硬编码 pm 的 bug 已修）
   → 每条完成后段间补扫自动接续（不漏）
单条 20~35 分钟；3 条约 60~90 分钟
```

---

## 3. 阶段 B：pm（**全新制作身份** → 绑定 → 3 条素材）

```
① 拷 pm 的 ROM：accept6_source\_id\0818_face_rom_pm_001_9 → accept6_inbox\_id\
   → 自动导入（28MB，约 30~60 秒；进度条按体积估算，与实测吻合）
   成功标志：日志"ID 素材就绪: 0818_face_rom_pm_001_9"；控制台 [ID] 行出现

② 制作 pm 身份（UE 内人工，3~5 分钟）：
   在 UE 内容浏览器打开刚导入的 pm ROM 的 Identity 资产
   → conform / Prepare / 提交 → 保存
   ★ 记住保存后的资产路径（下一步要填）

③ 绑定 pm：
   面板 身份区：
     资产目录 = pm 身份所在目录（如 /Game/CaptureManager/ID4 或你保存的目录）
     [检测] → 身份资产框填入最新身份（核对是否为 pm 那个；不是就手改路径）
     绑定目标 = pm  → [确认绑定]
   日志："身份绑定：<pm 身份路径> -> 分组 'pm'"

④ 拷 pm 三条 → accept6_inbox\pm\ → 依次自动解算 → FBX
```

> **两个身份共存时**：绑定一定用"身份资产"框显式指定（[检测] 只取目录内最新的），
> 并看清日志里 `-> 分组 'am'` / `'pm'` 是否对应正确。

---

## 5. 验证清单（严格）

跑完后逐条核对（**括号里是怎么查**）：

| # | 验证点 | 判据 |
|---|---|---|
| 1 | ROM 自动导入 | 控制台"ID 素材就绪"（或面板 ID 状态条） |
| 2 | 绑定路由正确 | `Saved\Config\MetaHumanSolver\stream_bindings.json` 里 `am` 与 `pm` 指向**各自**身份，不串 |
| 3 | 入队身份快照 | 队列 `queue_state.json` 中 am 任务 `identity`=am 身份、pm 任务=pm 身份 |
| 4 | pending → 冲刷 | 绑定前 am 素材显示"等待身份绑定"，绑定后自动入队 |
| 5 | 解算身份正确 | 日志"解算: xxx（identity: /Game/ID/...）"与该组绑定一致 |
| 6 | 阻塞期不丢素材 | 解算中拷入的素材最终都被处理（无遗漏） |
| 7 | 进度显示唯一 | 控制台"当前进度"有进度条，"处理中"只有一行状态（本轮修复） |
| 8 | 快捷键 | 控制台前台按 T 置顶切换生效、按 O 弹出 accept4_out |
| 9 | FBX 落盘 | `accept4_out\` 出现每个 take 的 .fbx |
| 10 | 失败区块 | 若某条失败，控制台红色"已失败"区显示原因（不再伪装"未监听"） |

---

## 6. 出问题时的检查点

| 现象 | 先看什么 |
|---|---|
| 拷入无反应 | ①是否已[启动监听] ②控制台[盘]行 ③是否卡在"等待身份绑定" |
| 队列 0 但素材在 | 绑定目标选错（如绑了根，素材在 pm 组 → 会兜底用根，也会入队）；或素材未就绪（3 秒稳定期+5 秒轮询，≤8 秒） |
| 绑定后没动 | 看日志"绑定确认: 分组 'x'"，确认组名与素材所在目录一致 |
| 解算失败 | 控制台"已失败"区的错误原文 + `Saved\Logs\MH_H.log` |
| 面板冻结 | 正常（解算中），看控制台"当前进度" |

---

## 7. 时间预估

| 阶段 | 耗时 |
|---|---|
| 清状态 + 配置 + 启动 | 5 分钟 |
| am ROM 导入 | 1~2 分钟 |
| am 身份制作（人工） | 3~5 分钟 |
| am 素材 ×1 解算 | 20~35 分钟（×2 若两条都跑，可并行观察排队） |
| pm 绑定 + 素材 ×1 解算 | 20~35 分钟 |

**建议**：先跑 am 一条 → 验证 1~5、7、9 项；通过后再跑 pm 条 → 验证双身份不串（第 2、5 项）。
