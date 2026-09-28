# 手动全流程测试 SOP（fulltest 实例）

一次**从头到尾由你亲手操作**的完整验收：启动 → 拷素材 → 自动导入/解算 → FBX 落盘 → 中途补扫 → 暂停/停止。
目标：证明"人只负责拷素材和启动，其余全自动"。

---

## 0. 环境（已备好，零点击起点）

```
素材源（你从这里"复制"）：
  E:\Work\Mobu_maya\MetaSol\spike\fulltest_source\am\
    event__emphasis_peak_lock__lock_emp_003_001            185MB   ← ① 先拷这条
    prosody__intonation__final_confirm_rise__int_001_b_001 201MB   ← ② 解算中拷（测补扫）
    emotion__fear__high_readable__hr_fea_012_003           216MB   ← ③ 可选，再测一次

热文件夹（拷到这里）：
  E:\Work\Mobu_maya\MetaSol\spike\fulltest_inbox\{am,pm,_id}\     ← 当前全空
FBX 输出：
  E:\Work\Mobu_maya\MetaSol\spike\fulltest_out\                   ← 当前 0 个文件
实例配置：
  E:\Work\Mobu_maya\MetaSol\deploy\tools\pipeline.config.fulltest.json
身份绑定（已就绪，无需再做）：
  am → /Game/CaptureManager/ID4/0818_face_rom_am_001_11
```

已清空：队列 / 状态 / 进度 / 停止哨兵 → **全新起点**。

---

## 1. 启动 GUI（用 fulltest 配置）

```
1) 关掉现在开着的 GUI（它在看 d1_line 配置）
2) 双击或命令行：
   "E:\Work\Mobu_maya\MetaSol\deploy\ui\src\MhsPipeline.App\bin\Debug\net9.0-windows\MhsPipeline.App.exe" "E:\Work\Mobu_maya\MetaSol\deploy\tools\pipeline.config.fulltest.json"
```

**核对（左栏）**：
- 工程：`MH_H.uproject`
- 路径：加载路径 `…\fulltest_inbox` · 身份导入 `/Game/CaptureManager/ID4` · 素材导入 `/Game/CaptureManager/AutoFT` · 导出 `…\fulltest_out`
- 身份与绑定 → 身份分组：`am/ → /Game/CaptureManager/ID4/0818_face_rom_am_001_11`（绿）

---

## 2. 启动引擎 → 观察空闲态

点 **[启动 / 继续]**（引擎控制区）。

**应该看到**：
- 引擎控制：`运行中`
- 实时部分 → 状态卡：🟢`监听中` + `队列 0 · 交付 0 · 失败 0` + 右上 `Ns前`
- 五个模块常显，均为 `0 条` + 空态提示行
- 进度卡：**不出现**（无活跃任务）
- 编辑器窗口会**自动移出屏幕**（无头形态），Web 监控：`http://127.0.0.1:8903`

---

## 3. 拷入第一条素材 → 观察全自动

把 `fulltest_source\am\event__emphasis_peak_lock__lock_emp_003_001`
复制到 `fulltest_inbox\am\`（整目录复制）。

| 时刻 | 你应该看到 |
|---|---|
| ≤15 秒 | **加载中** `[盘] am/event__… 已到达 · 等待接管`（琥珀）→ 随即变为 `待解算` |
| 约 1 分钟内 | **处理中** `[表演] am/event__… 导入素材中`（蓝）；进度卡（蓝框）出现：`… 导入转换 x%`，口径行注明"按素材体积估算" |
| 导入完成（185MB ≈ 2~3 分钟） | 处理中变为 `深度解算中`；进度卡显示 `Pass 1/3 · x/y 帧 · 已跑 … · 剩余约 …` |
| 解算中（20~30 分钟） | 进度条与帧数**每秒刷新**；ETA 蓝色显示在状态卡 |
| 完成 | **已完成** +1（绿），日志出现 `完成 交付 event__…（xxxxs）`；`fulltest_out\` 出现 `event__…fbx`（约 2.4MB） |

---

## 4. 中途补扫测试（关键：阻塞期到达不漏）

**在第一条还在解算时**，把第二条
`prosody__intonation__final_confirm_rise__int_001_b_001`
复制到 `fulltest_inbox\am\`。

**应该看到**：
- **加载中** 立刻出现 `[盘] am/prosody__… 已到达 · 等待接管（当前任务完成后自动处理）`（琥珀）
- 第一条解算+导出完成后，**无需任何操作**，第二条自动接续（处理中出现它）
- 这就是"解算阻塞 30 分钟期间到达的素材不丢失"的验证点

（可选：再拷第三条 `emotion__fear…` 再验一次）

---

## 5. 验收清单（勾满即全链路通过）

- [ ] 启动后空闲态正确（无进度卡、五模块 `0 条`）
- [ ] 拷入 ≤15s 被发现并出现在加载中
- [ ] 导入进度按体积估算、口径说明可见
- [ ] 解算进度卡有 Pass / 帧 / 已跑 / 剩余
- [ ] 解算中途拷入的素材被显示且自动接续
- [ ] 每条完成 → 已完成 +1（绿）→ FBX 落 `fulltest_out\`
- [ ] 日志按级别着色：完成绿 / 运行蓝 / 待处理琥珀 / 失败红
- [ ] 全程**未手动点过任何"处理"按钮**

预期产物：`fulltest_out\` 下 2~3 个 `.fbx`（每条约 2.4MB）。

---

## 6. 结束与停止

- **暂停**（跑完当前条后退出，队列保留）：GUI [暂停] 或
  `python launcher.py stop --config …\pipeline.config.fulltest.json`
- **强制结束**（立即杀）：GUI [强制结束]
- **看状态**：`deploy\tools\查看状态.bat`（默认看 d1_line 配置，看 fulltest 需加 `--config`）

---

## 7. 出问题怎么拿证据

| 现象 | 取什么 |
|---|---|
| 拷入没反应 | GUI 实时部分截图 + `fulltest_inbox` 目录树 |
| 卡在某阶段 | 进度卡截图（Pass/帧/已跑）+ `Saved\Logs\MH_H.log` 尾部 |
| 失败 | 红框"已失败"行的原因 + 日志 |
| watchdog 是否活着 | `pipeline.status.json` 的 `updated_at` 应每 5 秒变一次 |

常用取证路径：
```
F:\CFH\CFH\20260709\MH_H\Saved\Config\MetaHumanSolver\   （queue/state/progress/status/bindings）
F:\CFH\CFH\20260709\MH_H\Saved\Logs\MH_H.log
E:\Work\Mobu_maya\MetaSol\spike\fulltest_out\
```
