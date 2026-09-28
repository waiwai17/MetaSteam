# MetaSol / MetaSteam

**MetaHuman 流式解算流水线**：把"录完就拷文件夹"变成"拷完自动出 FBX"。
拷素材进热文件夹 → 自动导入 → 深度解算 → 导出 FBX，全程**零点击**，并自带崩溃自愈与实时面板。

## 它解决什么

- 素材按**上午(am) / 下午(pm)**（或任意自建分组）自动分流，各组用**各自演员的身份**解算
- 解算阻塞期间新拷入的素材**不漏**（段间补扫自动接续）
- 引擎崩了 **watchdog 自动拉起续跑**（队列与绑定不丢）
- 两条监控通道：**WPF 面板**（只读监控 / 身份绑定 / 工程切换）与 **Web 面板**（手机、局域网可看）

## 目录结构

```
deploy/
  Python/MetaHumanSolverEngine/   引擎 Python 包（流式管线全部逻辑，跑在 UE 进程内）
  tools/                          launcher.py（启动/停止/预检/watchdog）、bat 启动器
  ui/                             WPF 监控面板（.NET 9）+ 单元测试
  MetaHumanSolver/                插件源码 + 预编译 DLL（免编译分发）
  template/MH_Template/           模板工程（[新建] 就是复制它，免编译）
  assets/                         MetaHuman 公共资产包（本机生成，不入库）
tests/fixtures/                   轻量测试集（am/pm 两条 ID 素材 + 组装脚本）
docs/                             设计说明、SOP、干净机器部署
spike/test_*.py                   回归测试（python run_all_tests.py）
```

## 快速开始（干净机器）

前置：**UE 5.7（含 MetaHuman 插件）+ Python 3.9+**。然后：

```bash
git clone https://github.com/waiwai17/MetaSteam.git && cd MetaSteam
python tests/fixtures/make_testset.py --out D:\MetaSol\TestLine_inbox   # 准备 am/pm 测试素材
cd deploy/tools
python launcher.py init --name MH_Line01 --dir D:\MetaSol               # 探测UE→建工程→写配置
python launcher.py doctor --config pipeline.config.json                 # 预检应 errors: []
启动管线.bat                                                             # 跑起来
```

细节见 [`docs/干净机器部署.md`](docs/干净机器部署.md)；完整人工验收见 [`docs/手动全流程测试SOP.md`](docs/手动全流程测试SOP.md)。

## 工作流（日常）

1. 拷贝素材到 `inbox\<分组>\`（整目录，含 `take.json`）
2. 首次使用某演员：拷 ROM 到 `inbox\_id\` → 在 UE 里做身份 → GUI 里把身份绑到该分组
3. 之后该分组的素材全自动处理，FBX 落到 `fbx_output\<身份名>\<take>.fbx`

## 设计纪律

- **UI 只读**：面板绝不写队列（只写配置与绑定），所有进度一律来自 json 快照
- **不假报进度**：解算段用引擎真实帧级数据；导入段按体积估算并标注"估算"
- **幂等**：导入可跳过已完成的，导出自动清理残留资产 → 同一素材重跑安全
- **失败不循环**：失败任务只在其素材目录被更新后自动重排（防无限重试）

## 测试

```bash
cd spike && python run_all_tests.py           # Python 侧（12 套）
cd deploy/ui && dotnet test                    # C# 侧（46 项）
```

## 许可/资产说明

仓库**不含**：UE 引擎本身、MetaHuman 角色资产（`deploy/assets/`，体积 2GB 且受 MetaHuman 许可约束，
用 `deploy/tools/fetch_assets.py --from <参考工程>` 在本机生成）、原始拍摄素材与 FBX 产物。
入库的测试集只有两条轻量 ID（ROM）素材，便于验证身份导入与绑定。
