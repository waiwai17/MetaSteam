# spike · 测试目录说明

## 一键回归

```powershell
python run_all_tests.py              # 全部合成套件（8 套，约 1 分钟）
python run_all_tests.py --realdata   # 追加真实数据套件（87 take，需 Facial_Data 在位）
```

## 测试套件（保留在 spike/ 根）

| 脚本 | 覆盖 | 说明 |
|---|---|---|
| `test_detection_chain.py` | **全局检测链** | 一条龙：发现(就绪/分组/ID/幂等) → 身份解析(组→根兜底→pending→降级) → 执行链路(mock IO) → 状态聚合(全契约字段) → job 指纹重建。53 断言，专抓跨模块接缝回归 |
| `test_batch1.py` | 队列 + 就绪规则 | 六态状态机/原子写/幂等 |
| `test_batch2.py` | 扫描器 + 入队器 | 含 W3-6 根组兜底六项 |
| `test_batch4_unit.py` | 观测层单元 | 聚合/ETA/事件/并发零撕裂 |
| `test_batch4_contract.py` | 数据契约 | dashboard 引用字段比对 + `stream_queue_test.json` 真实队列冒烟（该 json 必须留在本目录） |
| `test_batch4_web.py` | Web 端到端 | 三端点/404/405/目录穿越/0.0.0.0/并发 |
| `test_batch53.py` | job 指纹 + 绑定直写 | "队列恒 0"事故根因回归 |
| `test_batch54.py` | e011 事故回归 | 失败复活/mtime 防循环/目录预检 |
| `test_batch*_realdata.py` | 真实数据 | 87 take 扫描分组入队（只读 Facial_Data） |
| `test_p1_loading.py` | 加载链路 | P1 阶段验收（历史保留） |

## 当前测试环境

| 目录 | 用途 |
|---|---|
| `accept4_source/` | 素材源（am ROM + am×2 + pm×1）——**测试时从这里复制，勿剪切** |
| `accept4_inbox/` | 热文件夹（面板"加载路径"指向这里） |
| `accept4_out/` | FBX 输出 |

## 归档（`_archive/`，2.6 GB，全部历史可追溯）

| 子目录 | 内容 |
|---|---|
| `outputs/` | 历次回归的 txt 结果、日志 |
| `state/` | 旧队列/状态/绑定 json、demo_web、dashboard 原型 |
| `scripts/` | 一次性编辑器驱动脚本（`_test_stream_*.py` / `_live_sim_*.py` / `_demo_server.py` 等 11 个） |
| `media/` | accept1~3 / live_sim / console_smoke 等旧测试环境（含已交付 FBX 证据） |

> 归档不删除：accept3_out 等目录含历史交付 FBX（验收证据）；确认不再需要时可整目录删除释放空间。
