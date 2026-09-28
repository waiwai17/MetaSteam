"""批处理调度器（深度单模式，非阻塞回调驱动 / 深度默认阻塞）。

流程：
  run_batch(json_path)
    -> config.load + versions 自检
    -> discover_shots（可选导入 -> 扫描 FootageCaptureData）
    -> limit / exclude 过滤
    -> 断点续跑 + 幂等过滤（skip_done）
    -> 逐条 run_depth_shot（默认阻塞，回调推进）
    -> _on_shot_done: 导出 AS + LS + ControlRig FBX -> 状态落盘 -> 下一条 -> [MHS_DONE]

dry_run(json_path)：预检，扫描素材 + 校验依赖 + 输出待跑清单，不实际解算。
cancel()：置标志，当前段解算完即停（引擎无中途硬取消 API）。

错误隔离：每条 try/except，一条失败不中断整批，跑完输出 ok / failed 清单。
断点续跑：batch_state.json 记录每条 done/failed，崩溃重启跳过 done。
幂等跳过：无状态记录时，若 Performance + AS 资产都已产出则视为 done 跳过。
"""

import datetime
import json
import os
import time

import unreal

from . import progress, pipeline, versions
from .config import load

_state = {
    "running": False,
    "cancel": False,
    "cfg": None,
    "items": [],
    "index": 0,
    "ok": 0,
    "failed": [],
    "json_path": "",
    "shots": {},       # {素材名: done/failed}
    "timings": [],     # 每条耗时（秒），用于剩余预估
    "shot_start": 0.0,
    "manifest": [],    # 交付清单（每段一条：交付物/状态/耗时），任务结束写盘 txt+json
}


def cancel():
    _state["cancel"] = True
    progress.warn("已请求取消，当前段解算完即停")


# ============ 批量状态（断点续跑 + 幂等） ============

def _state_path(json_path: str) -> str:
    return json_path.rsplit(".", 1)[0] + "_state.json"


def _load_state(json_path: str) -> dict:
    p = _state_path(json_path)
    if not os.path.exists(p):
        return {}
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f).get("shots", {})
    except Exception:
        return {}


def _save_state(json_path: str, shots: dict):
    data = {
        "updated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "shots": shots,
    }
    try:
        with open(_state_path(json_path), "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
    except Exception as exc:
        progress.warn("批量状态落盘失败: {}".format(exc))


def _mark_shot(name: str, status: str):
    _state["shots"][name] = status
    _save_state(_state["json_path"], _state["shots"])


def _fbx_export_path(cfg: dict, item: dict) -> str:
    """计算本段应产出的 FBX 磁盘路径（与 _on_shot_done 中导出路径保持一致）。

    深度/视频 -> {take名}.fbx（交付标准：Anims_Male_Ul_Enter_002_5_v2.fbx——磁盘 take 名
    剥日期前缀、保留 _vN 版本后缀）；音频烘焙 -> AS_{asset}.fbx。未启用 FBX 导出的模式返回空串。
    take 名来源按模式分流（rpartition 剥尾数字对两种输入语义不同）：
    - 混合模式：item["name"] 即磁盘 take 目录名（identifier）——剥 {yyyymmdd}_ 日期前缀
      （_delivery_name），其余原样保留（_vN 及 Gun_..._Idle_002 的 002 都是名字本身，禁止剥尾）
    - run_batch 模式：item["name"] 是 CD 资产名（CD_前缀 + TakeNumber 后缀）——
      剥 CD_ 与末尾 TakeNumber（_5）
    """
    asset_name = _asset_name(cfg, item["name"])
    if item["type"] in ("depth", "video"):
        if not cfg.get("export_fbx"):
            return ""
        if cfg.get("import_then_solve"):
            core = _delivery_name(item["name"])
        else:
            core = _take_core_name(item["name"])
        return "{}/{}.fbx".format(pipeline.fbx_asset_dir(cfg, asset_name), core)
    # audio
    if not cfg.get("export_baked_fbx"):
        return ""
    return "{}/AS_{}.fbx".format(pipeline.fbx_asset_dir(cfg, asset_name), asset_name)


def _fmt_duration(seconds: float) -> str:
    """秒 -> 人读时长（mm:ss / h:mm:ss）。"""
    s = max(0, int(round(seconds)))
    if s < 3600:
        return "{}:{:02d}".format(s // 60, s % 60)
    return "{}:{:02d}:{:02d}".format(s // 3600, (s // 60) % 60, s % 60)


def _fmt_bytes(num_bytes: int) -> str:
    """字节 -> 人读大小（KB/MB）。"""
    if num_bytes <= 0:
        return "-"
    if num_bytes < 1024 * 1024:
        return "{:.0f} KB".format(num_bytes / 1024.0)
    return "{:.1f} MB".format(num_bytes / (1024.0 * 1024.0))


def _manifest_add(cfg: dict, item: dict, success: bool, elapsed: float, error: str = ""):
    """收集一条交付清单记录（_on_shot_done / 导入段收尾调用）。

    条目含交付物路径（FBX 磁盘路径+大小、AS/LS 资产路径），失败时带原因。
    """
    try:
        entry = {
            "name": item["name"],
            "type": item.get("type", ""),
            "success": bool(success),
            "elapsed_seconds": round(max(0.0, elapsed), 1),
            "delivery": {"fbx": "", "fbx_bytes": 0, "as": "", "ls": ""},
            "error": error or "",
            "time": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        }
        if success:
            asset_name = _asset_name(cfg, item["name"])
            entry["delivery"]["as"] = "{}/AS".format(pipeline._asset_dir(cfg, asset_name))
            entry["delivery"]["ls"] = pipeline.ls_asset_path(cfg, asset_name)
            fbx_path = _fbx_export_path(cfg, item)
            if fbx_path:
                entry["delivery"]["fbx"] = fbx_path
                try:
                    entry["delivery"]["fbx_bytes"] = os.path.getsize(fbx_path) if os.path.exists(fbx_path) else 0
                except OSError:
                    entry["delivery"]["fbx_bytes"] = 0
        _state["manifest"].append(entry)
    except Exception as exc:
        progress.warn("交付清单记录失败: {}".format(exc))


def _write_manifest():
    """写盘交付清单（每段完成后覆盖写，最终完整）：txt（人读）+ json（机读）双份。

    位置：fbx_output_dir 根目录（与 FBX 交付物同处，随交付一起拿走）；
    未配置 fbx_output_dir 时回落项目 Saved/Config/MetaHumanSolver。
    文件名用批次时间戳（run_next 首段 / run_batch 开始生成），同批次多次写盘为覆盖更新。
    """
    cfg = _state.get("cfg") or {}
    items = _state["manifest"]
    out_dir = pipeline.fbx_asset_dir(cfg) or "{}/Config/MetaHumanSolver".format(
        unreal.Paths.project_saved_dir().replace("\\", "/"))
    try:
        os.makedirs(out_dir, exist_ok=True)
    except Exception as exc:
        progress.warn("交付清单目录不可创建: {} ({})".format(out_dir, exc))
        return

    stamp = _state.get("manifest_stamp") or datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    ok_count = sum(1 for e in items if e["success"])
    summary = {"ok": ok_count, "failed": len(items) - ok_count, "total": len(items)}
    started = items[0]["time"] if items else "-"

    # json（机读）
    payload = {
        "job": cfg.get("job_name") or os.path.basename(_state.get("json_path") or ""),
        "created_at": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "started_at": started,
        "fbx_output_dir": out_dir.replace("\\", "/"),
        "summary": summary,
        "items": items,
    }
    try:
        with open(os.path.join(out_dir, "delivery_{}.json".format(stamp)), "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, ensure_ascii=False)
    except Exception as exc:
        progress.warn("交付清单 json 写盘失败: {}".format(exc))

    # txt（人读）
    try:
        lines = ["== MetaHumanSolver 交付清单  {} ==".format(started)]
        for i, e in enumerate(items, 1):
            mark = "✓" if e["success"] else "✗"
            lines.append("[{}/{}] {}  {}  {}".format(i, len(items), e["name"], mark, _fmt_duration(e["elapsed_seconds"])))
            if e["success"]:
                d = e["delivery"]
                if d.get("fbx"):
                    lines.append("    FBX  {}  ({})".format(d["fbx"], _fmt_bytes(d.get("fbx_bytes", 0))))
                if d.get("as"):
                    lines.append("    AS   {}".format(d["as"]))
                if d.get("ls"):
                    lines.append("    LS   {}".format(d["ls"]))
            elif e.get("error"):
                lines.append("    原因 {}".format(e["error"]))
        lines.append("---")
        lines.append("完成 {} / 失败 {} / 共 {}".format(summary["ok"], summary["failed"], summary["total"]))
        with open(os.path.join(out_dir, "delivery_{}.txt".format(stamp)), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    except Exception as exc:
        progress.warn("交付清单 txt 写盘失败: {}".format(exc))


def _take_core_name(cd_name: str) -> str:
    """CD 资产名 -> 原始 take 名（交付命名标准）。

    实测形态：CD_Anims_Flint_Ul_Enter_005_5 -> Anims_Flint_Ul_Enter_005
    （剥 CD_ 前缀 + 末尾 TakeNumber 序号；序号只在其前有内容时剥，避免误伤纯数字名）。
    """
    name = cd_name[len("CD_"):] if cd_name.startswith("CD_") else cd_name
    head, sep, tail = name.rpartition("_")
    if sep and head and tail.isdigit():
        name = head
    return name


def _delivery_name(take_name: str) -> str:
    """交付命名（混合模式）：剥磁盘 take 目录名的 {yyyymmdd}_ 日期前缀。

    磁盘 identifier 形态 = {日期}_{slate}_{take}[_vN]（20260824_Anims_xxx_5_v2），
    日期前缀是归档噪音，交付物（UE 存储目录 + FBX 文件名）统一剥掉；
    _vN 版本后缀保留（同名 take 重录的区分依据，用户交付标准）。
    无日期前缀的旧形态（Anims_Flint_Ul_Enter_005）原样返回，历史命名不受影响。
    """
    if len(take_name) > 9 and take_name[:8].isdigit() and take_name[8] == "_":
        return take_name[9:]
    return take_name


def _is_shot_done(cfg: dict, item: dict) -> bool:
    """幂等判断：Performance + AS 资产存在，且 FBX 交付物已在磁盘生成，才算完成。

    若仅 Performance/AS 存在而 FBX 缺失（如上次 FBX 导出失败但状态落盘为 done），
    不视为完成，重跑时会重新导出 FBX，避免交付物静默缺失。
    FBX 命名曾为 MH_FaceAnimation_{asset}.fbx（旧），现为 {take名}.fbx（新）——
    任一存在即视为已交付（旧产物重命名过渡期不触发重跑）。
    """
    asset_name = _asset_name(cfg, item["name"])
    perf_path = pipeline.perf_asset_path(cfg, asset_name)
    as_path = "{}/AS".format(pipeline._asset_dir(cfg, asset_name))
    asset_ok = (unreal.EditorAssetLibrary.does_asset_exist(perf_path)
                and unreal.EditorAssetLibrary.does_asset_exist(as_path))
    if not asset_ok:
        return False
    # FBX 交付物必须是最终的硬性完成条件（磁盘文件必须真实存在）
    fbx_path = _fbx_export_path(cfg, item)
    if fbx_path:
        legacy_path = "{}/MH_FaceAnimation_{}.fbx".format(
            os.path.dirname(fbx_path), asset_name)
        return os.path.exists(fbx_path) or os.path.exists(legacy_path)
    return True


def _filter_done(json_path: str, cfg: dict, items: list) -> list:
    """断点续跑 + 幂等：过滤已完成的素材，返回待处理列表。"""
    shots = _load_state(json_path)
    pending = []
    skipped = []
    for item in items:
        name = item["name"]
        if item.get("batch_import"):
            # 批次导入段：全部 take 已导入即视为完成（断点重跑不重导）
            if all(_import_already_done(cfg, {"name": n, "batch": b})
                   for n, _d, b in item.get("takes", [])):
                shots[name] = "done"
                skipped.append(name)
                continue
            pending.append(item)
            continue
        status = shots.get(name)
        if status == "done":
            skipped.append(name)
            continue
        if status is None and _is_shot_done(cfg, item):
            shots[name] = "done"  # 幂等补记
            skipped.append(name)
            continue
        pending.append(item)
    if skipped:
        shown = ", ".join(skipped[:5]) + ("..." if len(skipped) > 5 else "")
        progress.log("已跳过 {} 段（已完成）: {}".format(len(skipped), shown))
    _save_state(json_path, shots)
    _state["shots"] = shots
    return pending


def _apply_limit(items: list, cfg: dict) -> list:
    """job.limit > 0 时只跑前 N 条（分批测试用）。"""
    limit = int(cfg.get("limit") or 0)
    if limit > 0 and len(items) > limit:
        progress.log("limit={} 生效：从 {} 段截取前 {} 段".format(limit, len(items), limit))
        return items[:limit]
    return items


def _apply_exclude(items: list, cfg: dict) -> list:
    """job.exclude 排除指定素材（格式不兼容/不想跑的）。

    兼容两种填法：CD 资产名（CD_face_am01_8，解算扫描语义）或 take 目录名
    （face_am01_8，混合段/导入段语义）——排除 ROM 时两种写法都生效。
    """
    excludes = set(cfg.get("exclude") or [])
    if not excludes:
        return items
    kept = []
    for i in items:
        if i.get("batch_import"):
            # 批次导入段：从 takes 列表里剔除被排除的 take，整段被排空则删除
            kept_takes = [(n, d, b) for n, d, b in i.get("takes", [])
                          if n not in excludes and ("CD_" + n) not in excludes]
            if kept_takes:
                i = dict(i)
                i["takes"] = kept_takes
                kept.append(i)
            else:
                progress.log("exclude 生效：批次导入段全部排除: {}".format(i["name"]))
            continue
        if i["name"] not in excludes and ("CD_" + i["name"]) not in excludes:
            kept.append(i)
    dropped = len(items) - len(kept)
    if dropped:
        progress.log("exclude 生效：排除 {} 段".format(dropped))
    return kept


# ============ 预检 dry-run ============

# 阶段 A 导入预检：take 目录必需文件（缺任一即报错，导入会失败或不完整）。
# 音频非必需（深度解算纯视觉跟踪；无音频=无 SW_Audio_ 资产与舌头追踪，解算正常）
_IMPORT_REQUIRED_FILES = ("take.json", "depth_data.bin", "depth_metadata.mhaical")
_IMPORT_AUDIO_FILES = ("audio_metadata.json",)


def _is_take_dir(directory: str) -> bool:
    """take 目录判定（与 C++ 导入器语义一致：take.json 或 *.cptake）。"""
    try:
        if os.path.isfile(os.path.join(directory, "take.json")):
            return True
        return any(f.endswith(".cptake") for f in os.listdir(directory))
    except OSError:
        return False


def discover_takes(source_dir: str) -> list:
    """层级自适应的 take 发现——用户怎么填都对。

    返回 [(take名, take目录, 批次名)]：
    - source 本身是 take（ROM 单导入）  -> 批次 ""（导入 capture_root/{take}，无批次层）
    - source 下直接是 take（填批次目录）-> 批次 = source 目录名（如 face_am01）
    - source 下是批次目录（填数据根）  -> 批次 = 一级子目录名（如 face_am01/face_pm01）
    三种填法殊途同归：镜像结构 capture_root/{批次}/{take}，批次名一致。
    """
    source = (source_dir or "").rstrip("/\\")
    if not source or not os.path.isdir(source):
        return []
    if _is_take_dir(source):
        return [(os.path.basename(source), source, "")]

    results = []
    source_base = os.path.basename(source)
    for name in sorted(os.listdir(source)):
        child = os.path.join(source, name)
        if not os.path.isdir(child):
            continue
        if _is_take_dir(child):
            results.append((name, child, source_base))
        else:
            for sub in sorted(os.listdir(child)):
                sub_dir = os.path.join(child, sub)
                if os.path.isdir(sub_dir) and _is_take_dir(sub_dir):
                    results.append((sub, sub_dir, name))
    return results


def _check_take_files(take_dir: str) -> tuple:
    """核对单个 take 目录：返回 (必需缺失清单, 是否缺音频)。

    音频缺失不阻断（深度解算纯视觉；仅少 SW_Audio_ 资产与舌头追踪），预检时提示。
    """
    missing = [name for name in _IMPORT_REQUIRED_FILES
               if not os.path.isfile(os.path.join(take_dir, name))]
    try:
        if not any(f.lower().endswith(".mov") for f in os.listdir(take_dir)):
            missing.append("*.mov")
        has_audio_meta = os.path.isfile(os.path.join(take_dir, _IMPORT_AUDIO_FILES[0]))
        has_wav = any(f.lower().endswith(".wav") for f in os.listdir(take_dir))
        audio_missing = not (has_audio_meta or has_wav)
    except OSError:
        missing.append("*.mov")
        audio_missing = False
    return missing, audio_missing


def precheck_import(json_path: str) -> dict:
    """阶段 A 导入预检：纯磁盘扫描（不碰 UE），秒级完成。

    检查：批次枚举 / 每 take 必需文件 / 疑似 ROM 识别（take 目录名以批次名开头）/
    媒体输出根可创建。结果走 progress 日志（[MHS_LOG]/[MHS_ERROR]，UI 日志框直接可见）。
    检出任何缺失时返回 errors 非空——先修数据再导入，避免 37 分钟导入白跑。
    """
    try:
        cfg = load(json_path)
    except Exception as exc:
        progress.error("job.json 读取失败: {}".format(exc))
        return {"errors": [str(exc)]}

    source = cfg.get("footage_source_dir")
    if not source:
        progress.error("未配置 footage_source_dir（数据根目录）")
        return {"errors": ["no_source"]}
    if not os.path.isdir(source):
        progress.error("数据根目录不存在: {}".format(source))
        return {"errors": ["bad_source"]}

    # 媒体输出根可创建性（导入时必须可写）
    errors = []
    media_root = cfg.get("media_output_root")
    if media_root:
        try:
            os.makedirs(media_root, exist_ok=True)
        except Exception as exc:
            progress.error("媒体输出根不可创建: {} ({})".format(media_root, exc))
            errors.append("bad_media_root")
    else:
        progress.error("未配置 media_output_root（媒体转换输出根）")
        errors.append("no_media_root")

    progress.log("===== 导入预检: {} =====".format(source.replace("\\", "/")))

    # 层级自适应发现（discover_takes）：填单 take / 批次目录 / 数据根目录均可
    takes = discover_takes(source)
    if not takes:
        progress.error("目录下没有 take（判定标志：文件夹内含 take.json）: {}".format(source))
        return {"errors": errors + ["no_takes"]}

    total_takes = rom_takes = complete_takes = 0
    # 按批次分组输出（batch="" 归为导入根）
    display_batch = lambda b: b if b else "（无批次层）"
    grouped = {}
    for take_name, take_dir, batch in takes:
        grouped.setdefault(batch, []).append((take_name, take_dir))
    for batch_name in sorted(grouped):
        batch_takes = grouped[batch_name]
        progress.log("批次 {}: {} 个 take".format(display_batch(batch_name), len(batch_takes)))
        for take_name, take_dir in batch_takes:
            total_takes += 1
            # ROM 目录名实测形如 20260710_face_am01_8（日期前缀 + 批次名 + 序号），
            # 批次名在中间——用包含判定（仅标注"疑似"，真正排除靠 exclude）
            is_rom = bool(batch_name) and (batch_name in take_name)
            if is_rom:
                rom_takes += 1
            missing, audio_missing = _check_take_files(take_dir)
            tag = " [疑似ROM]" if is_rom else ""
            if missing:
                progress.error("  ├─ {}{} 缺失: {}".format(take_name, tag, ", ".join(missing)))
                errors.append(take_name)
            else:
                complete_takes += 1
                if audio_missing:
                    progress.warn("  ├─ {}{} 文件齐全（无音频：将无 SW_Audio_ 资产与舌头追踪）".format(
                        take_name, tag))
                else:
                    progress.log("  ├─ {}{} 文件齐全".format(take_name, tag))

    perf_count = total_takes - rom_takes
    progress.log("汇总: {} 批次 · {} take · 表演 {} · 疑似 ROM {} · 文件齐全 {}/{}".format(
        len(grouped), total_takes, perf_count, rom_takes, complete_takes, total_takes))

    # ── 并发报告（静态检测 + 配置校验 + 预期告知，不阻断）──
    concurrency_cfg = cfg.get("imports_concurrency", "auto")
    cfg_is_auto = not isinstance(concurrency_cfg, int) or concurrency_cfg <= 0
    cfg_valid = cfg_is_auto or (isinstance(concurrency_cfg, int) and concurrency_cfg > 0)
    try:
        info = list(unreal.MHSTakeImporter.get_recommended_import_concurrency(
            0 if cfg_is_auto else int(concurrency_cfg)))
        physical, logical, actual = info[0], info[1], info[2]
    except Exception as exc:
        progress.warn("并发信息获取失败（本机核数不可用）: {}".format(exc))
        physical = logical = actual = None

    if physical is not None:
        progress.log("并发报告: 本机物理核 {} / 逻辑核 {}".format(physical, logical))
        if not cfg_valid:
            progress.error("imports_concurrency 配置非法（应为 auto 或正整数）: {!r}".format(concurrency_cfg))
        else:
            if cfg_is_auto:
                progress.log("imports_concurrency=auto → 实际并发 {}（物理核/3，上限16）".format(actual))
            else:
                progress.log("imports_concurrency 显式值 {}（实际生效 {}）".format(concurrency_cfg, actual))
        if actual is not None:
            if total_takes and actual > total_takes:
                progress.warn("并发 {} 高于 take 数 {}：并发浪费，建议设 imports_concurrency={} 或保持 auto".format(
                    actual, total_takes, total_takes))
            if actual >= 16:
                progress.warn("并发已达上限 16；若数据盘为 HDD，IO 争抢会反噬收益，建议降低并发")
        progress.log("导入耗时预估: 首跑无单take转换基准，无法精确预估；跑完1个take后参考日志[② 转换诊断]的平均核占用与耗时")

    if errors:
        progress.error("预检发现 {} 个问题，请先修复数据再导入".format(len(errors)))
    else:
        progress.log("预检通过，可执行导入")
    return {"batches": len(grouped), "takes": total_takes, "performances": perf_count,
            "rom": rom_takes, "errors": errors}


def _check_dependencies(cfg: dict) -> list:
    """校验依赖资产存在 + 输出目录可写（只做存在性检查，不加载资产——
    加载 Identity 会触发 PredictiveSolver 初始化，预检不需要）。"""
    errors = []
    for key, path in (
        ("identity_path", cfg.get("identity_path")),
        ("skeleton_path", cfg.get("skeleton_path")),
        ("meta_human_class", cfg.get("meta_human_class")),
    ):
        if not path:
            continue
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            errors.append("{} 不存在: {}".format(key, path))
    # 批次级 identity 映射逐项校验（漏配/错路径提前暴露，避免解算 24 分钟后失败）
    for batch, identity_path in (cfg.get("identity_map") or {}).items():
        if not unreal.EditorAssetLibrary.does_asset_exist(identity_path):
            errors.append("identity_map[{}] 不存在: {}".format(batch, identity_path))
    out = pipeline.resolve_path(cfg.get("fbx_output_dir", ""))
    if out and not os.path.isdir(out):
        try:
            os.makedirs(out, exist_ok=True)
        except Exception:
            errors.append("fbx_output_dir 不可创建: {}".format(out))
    return errors


def dry_run(json_path: str) -> dict:
    """预检：扫描素材 + 校验依赖 + 输出清单，不实际解算。"""
    versions.check_supported()
    try:
        cfg = load(json_path)
    except Exception as exc:
        progress.error("job.json 读取失败: {}".format(exc))
        return {}

    items = _discover_shots(cfg)
    if not items:
        progress.error("未发现任何待处理素材")
        return {"items": [], "skipped": [], "errors": ["未发现素材"]}

    items = _apply_limit(items, cfg)
    items = _apply_exclude(items, cfg)
    errors = _check_dependencies(cfg)
    shots = _load_state(json_path)
    todo = []
    skipped = []
    for item in items:
        name = item["name"]
        if shots.get(name) == "done" or _is_shot_done(cfg, item):
            skipped.append(name)
        else:
            todo.append(name)

    progress.log("===== 预检结果 =====")
    progress.log("依赖检查: {}".format("通过" if not errors else "发现 {} 个问题".format(len(errors))))
    for e in errors:
        progress.error("  - {}".format(e))
    progress.log("待处理: {} 段".format(len(todo)))
    for item in items:
        if item["name"] in todo:
            progress.log("  [待跑] {} (identity: {})".format(
                item["name"], item.get("identity") or "<全局>"))
    progress.log("已跳过(完成): {} 段".format(len(skipped)))
    for n in skipped:
        progress.log("  [跳过] {}".format(n))

    # LS 交付链路预检（不解算）：对每个待处理素材确认 LS 复用/重建路径是否可用，
    # 提前暴露"解算完但 LS 导不出"的问题（避免 24 分钟解算后才失败）。
    for item in items:
        name = item["name"]
        if shots.get(name) == "done" or _is_shot_done(cfg, item):
            continue
        asset_name = _asset_name(cfg, name)
        ls_fixed = pipeline.ls_asset_path(cfg, asset_name)
        if unreal.EditorAssetLibrary.does_asset_exist(ls_fixed):
            progress.log("  [LS] {} 将复用固定路径 LS: {}".format(name, ls_fixed))
            continue
        found = pipeline._find_existing_level_sequence(cfg, asset_name)
        if found:
            progress.log("  [LS] {} 将复用扫描到的 LS: {}".format(name, found))
            continue
        mh = cfg.get("meta_human_class", "")
        if not mh:
            mh = pipeline._find_default_meta_human_class(asset_name)
        if mh:
            progress.log("  [LS] {} 将自动重建（角色蓝图: {}）".format(name, mh))
        else:
            progress.error("  [LS] {} 无既有 LS 且未找到角色蓝图 → 导出阶段将失败！"
                           "请配置 meta_human_class 或确认 /Game/MetaHumans 下有 BP_* 角色蓝图".format(name))

    return {"items": todo, "skipped": skipped, "errors": errors}


# ============ 批处理主流程 ============

def run_batch(json_path: str, skip_done: bool = True):
    if _state["running"]:
        progress.error("已有批处理在运行，请勿重复启动")
        return

    versions.check_supported()
    try:
        cfg = load(json_path)
    except Exception as exc:
        progress.error("job.json 读取失败: {}".format(exc))
        return

    # 仅导入模式：只把素材导入为 FootageCaptureData，不扫描不解算
    if cfg.get("import_only"):
        if cfg.get("import_footage") and cfg.get("footage_source_dir"):
            try:
                pipeline.import_footage(cfg)
                progress.done(1, [])
            except Exception as exc:
                progress.error("素材导入失败: {}".format(exc))
                progress.done(0, ["import"])
        else:
            progress.error("import_only=true 但未配置 import_footage / footage_source_dir")
        return

    items = _discover_shots(cfg)
    if not items:
        progress.error("未发现任何待处理素材")
        progress.done(0, [])
        return

    items = _apply_limit(items, cfg)
    items = _apply_exclude(items, cfg)
    total = len(items)
    if skip_done:
        items = _filter_done(json_path, cfg, items)
    else:
        _state["shots"] = {}

    if not items:
        progress.log("所有素材均已完成，无需处理")
        progress.done(0, [])
        return

    _state.update(running=True, cancel=False, cfg=cfg, items=items, index=0, ok=0,
                  failed=[], json_path=json_path, timings=[], shot_start=0.0)
    _state["manifest"] = []  # 新批次重置交付清单
    _state["manifest_stamp"] = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    progress.log("批处理开始: 待处理 {} 段（总计 {} 段）".format(len(items), total))
    _start_next()


def count_shots(json_path: str) -> int:
    """统计待处理总段数（用于 C++ 端预估剩余时间），不实际解算。失败返回 -1。"""
    try:
        cfg = load(json_path)
    except Exception:
        return -1
    items = _apply_limit(_discover_shots(cfg), cfg)
    items = _apply_exclude(items, cfg)
    return len(items)


def _finish_single_segment(success: bool, failed_names: list = None):
    """单段收尾（run_next 每段必经出口）：重置运行态并广播 [MHS_DONE]。

    状态机闭环纪律：run_next 开头置 running=True，则每条 return 路径必须经此函数
    重置——漏重置会让后续段全部被"已有批处理在运行"拒绝（C++ 等不到 DONE，
    按钮死灰）。语义与 _on_shot_done 的 single_segment 分支严格对齐。
    """
    failed = list(failed_names or [])
    if not success and failed:
        _state["failed"].extend(failed)
    progress.status("running" if success else "error")
    _state["running"] = False
    _state["cancel"] = False
    progress.monitor_batch_context(0, 0, -1.0)
    if _state.get("manifest"):
        _write_manifest()  # 单段收尾：交付清单覆盖写（最终段写完即完整版）
    progress.done(1 if success else 0, failed)


def run_next(index: int, json_path: str):
    """单段执行：只解算并导出第 index 段，完成后返回。

    供 C++ 端用定时器逐段驱动，实现段间 UI 刷新（深度解算必须阻塞，无法在段内
    让出游戏线程，但段间 Exec 返回后游戏线程空出，[MHS_PROGRESS] 日志得以被 UI 处理）。
    不影响 run_batch（整批，控制台场景）的正确性。
    """
    if _state["running"]:
        progress.error("已有批处理在运行，请勿重复启动")
        return

    versions.check_supported()
    try:
        cfg = load(json_path)
    except Exception as exc:
        progress.error("job.json 读取失败: {}".format(exc))
        progress.done(0, [])
        return

    items = _apply_limit(_discover_shots(cfg), cfg)
    items = _apply_exclude(items, cfg)
    total = len(items)
    if index < 0 or index >= total:
        progress.error("段索引越界: {}（共 {} 段）".format(index, total))
        progress.done(0, [])
        return

    item = items[index]

    # 新批次首段：重置交付清单（C++ 面板逐段驱动，每次 index=0 视为新批次）
    if index == 0:
        _state["manifest"] = []
        _state["manifest_stamp"] = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")

    # 导入段：
    #   batch_import=True（混合段/纯导入序列已合并）→ 批次并发导入全部 take（幂等过滤已导入项）；
    #   否则单 take 导入（兼容旧行为）。批次名来自 discover_takes 层级自适应。
    # 进度监控：任务级（无帧数据）——小窗显示"导入批次"+计时+批次 [i/N]。
    if item["type"] == "import":
        _state.update(running=True, cancel=False, single_segment=True,
                      cfg=cfg, items=items, index=index, ok=0,
                      failed=[], json_path=json_path, timings=[], shot_start=time.time())
        progress.status("running")
        progress.monitor_batch_context(0, 0, -1.0)

        if item.get("batch_import"):
            # ── 批次导入段：并发导入全部 take（imports_concurrency 生效）──
            pending = []  # [(batch, take_dir)]
            for take_name, take_dir, take_batch in item.get("takes", []):
                if not _import_already_done(cfg, {"name": take_name, "batch": take_batch}):
                    pending.append((take_batch, take_dir))
            if not pending:
                progress.log("批次已全部导入，跳过导入段: {}".format(item["name"]))
                progress.progress(index + 1, total, item["name"])
                progress.result(item["name"], True, 0.0)
                _finish_single_segment(True)
                return
            try:
                progress.monitor_begin_task("导入批次: {}".format(item["name"]))
                progress.log("导入批次 {}/{}: {} 个 take 待导入（并发按 imports_concurrency）".format(
                    index + 1, total, len(pending)))
                concurrency = versions.resolve_import_concurrency(cfg)
                by_batch = {}
                for take_batch, take_dir in pending:
                    by_batch.setdefault(take_batch, []).append(take_dir)
                for take_batch, dirs in by_batch.items():
                    package_root = cfg["capture_root"].rstrip("/")
                    if take_batch:
                        package_root = "{}/{}".format(package_root, take_batch)
                    progress.log("导入批次组 {}: {} 个 take 并发转换".format(take_batch or "(根)", len(dirs)))
                    # 任务进度上下文：小窗实时显示 x/y take · 并发n · 剩余（C++ 逐 take 更新）
                    progress.monitor_set_task_context(len(dirs), concurrency)
                    versions.import_take_list(dirs, cfg["media_output_root"], package_root, concurrency)
                progress.progress(index + 1, total, item["name"])
                progress.result(item["name"], True, time.time() - _state["shot_start"])
                progress.monitor_end(True)
                _finish_single_segment(True)
            except Exception as exc:
                progress.error("批次导入失败 {}: {}".format(item["name"], exc))
                progress.monitor_end(False)
                progress.progress(index + 1, total, item["name"])
                _finish_single_segment(False, [item["name"]])
            return

        # ── 单 take 导入（兼容旧行为/单 ROM 导入）──
        if _import_already_done(cfg, item):
            progress.log("take 已导入，跳过导入段: {}".format(item["name"]))
            progress.progress(index + 1, total, item["name"])
            progress.result(item["name"], True, 0.0)
            _finish_single_segment(True)
            return
        try:
            progress.monitor_begin_task("导入: {}".format(item["name"]))
            progress.log("导入段 {}/{}: {}".format(index + 1, total, item["name"]))
            package_root = cfg["capture_root"].rstrip("/")
            if item.get("batch"):
                package_root = "{}/{}".format(package_root, item["batch"])
            versions.import_single_take(item["dir"], cfg["media_output_root"], package_root)
            progress.progress(index + 1, total, item["name"])
            progress.result(item["name"], True, time.time() - _state["shot_start"])
            progress.monitor_end(True)
            _finish_single_segment(True)
        except Exception as exc:
            progress.error("导入段失败 {}: {}".format(item["name"], exc))
            progress.monitor_end(False)
            progress.progress(index + 1, total, item["name"])
            _finish_single_segment(False, [item["name"]])
        return

    # 解算段：混合模式下扫描本组批次子目录（capture_root/{批次}），否则全 capture_root。
    scan_cfg = cfg
    if cfg.get("import_then_solve") and item.get("batch"):
        scan_cfg = dict(cfg)
        scan_cfg["capture_root"] = "{}/{}".format(cfg["capture_root"].rstrip("/"), item["batch"])
    if "package_path" not in item:
        found = _find_footage_capture_data(scan_cfg, cfg["mode"])
        matched = _match_footage_item(found, item["name"])
        if matched is None:
            progress.error("解算段 {} 未找到已导入素材（检查导入段是否成功）".format(item["name"]))
            progress.progress(index + 1, total, item["name"])
            _finish_single_segment(False, [item["name"]])
            return
        item = dict(item)
        item["package_path"] = matched["package_path"]
        item["identity"] = pipeline.resolve_identity(scan_cfg, item["package_path"])

    # 幂等：该段已完成（Performance + AS + FBX 齐备）则直接跳过，避免重复解算浪费数十分钟。
    # 仍发 progress/done，让 C++ 端正常推进下一段（TotalShots 依赖 [MHS_PROGRESS] 建立）。
    if _is_shot_done(cfg, item):
        progress.log("段已完成，跳过解算: {}".format(item["name"]))
        progress.progress(index + 1, total, item["name"])
        progress.result(item["name"], True, 0.0)
        progress.done(1, [])
        return

    _state.update(running=True, cancel=False, single_segment=True,
                  cfg=cfg, items=items, index=index, ok=0,
                  failed=[], json_path=json_path, timings=[], shot_start=time.time())
    progress.status("running")
    # 批次上下文（表演级显示语义）：混合模式 [j/m] = 组内表演序号/总数（导入段不计数），
    # 并发 [MHS_DISPLAY] 驱动面板进度条（MHS_PROGRESS 保持段级仅供 C++ 推进）；
    # 其余模式维持段级 [i/N]
    if cfg.get("import_then_solve"):
        perf_count = sum(1 for s in items if s["type"] == "import")
        perf_total = max(1, total - perf_count)
        perf_no = max(1, index + 1 - perf_count)
        progress.monitor_batch_context(perf_no, perf_total, -1.0)
        progress.display(perf_no, perf_total, item["name"])
        progress.log("解算表演 {}/{}: {}".format(perf_no, perf_total, item["name"]))
    else:
        progress.monitor_batch_context(index + 1, total, -1.0)
        progress.log("单段执行 {}/{}: {}".format(index + 1, total, item["name"]))
    try:
        if item["type"] == "audio":
            pipeline.run_audio_shot(cfg, item, _on_shot_done)
        elif item["type"] == "depth":
            pipeline.ensure_identity_ready(cfg, item.get("identity", ""))
            pipeline.run_depth_shot(cfg, item, _on_shot_done)
        else:
            pipeline.ensure_identity_ready(cfg, item.get("identity", ""))
            pipeline.run_video_shot(cfg, item, _on_shot_done)
    except Exception as exc:
        progress.error("本段异常: {}".format(exc))
        # 原生监控收尾（begin 后异常的兜底；未激活时幂等无操作）
        progress.monitor_end(False)
        # 失败也发进度：否则 C++ 端 TotalShots 无法建立，首段失败会导致整批提前停止
        progress.progress(index + 1, total, item["name"])
        _finish_single_segment(False, [item["name"]])


def _discover_shots(cfg: dict):
    """按模式发现素材：音频=单 SoundWave；深度/视频=扫描 FootageCaptureData。

    import_then_solve=true（面板任务列表全自动模式）：返回混合段序列——
    先导入段（source 下每 take 一段，逐段 Exec 段间刷 UI），后解算段（每 take 一段）。
    批次名 = footage_source_dir 的末级目录名（如 E:/FACE/face_am01 -> face_am01），
    导入目标 capture_root/{批次名}，解算扫描同一目录，identity 用本组 identity_path。
    """
    if cfg["mode"] == "audio":
        sound_wave = unreal.load_asset(cfg["audio_path"])
        if sound_wave is None:
            progress.error("SoundWave 加载失败: {}".format(cfg["audio_path"]))
            return []
        return [{"type": "audio", "name": sound_wave.get_name()}]

    if cfg.get("import_footage") and cfg.get("footage_source_dir"):
        if cfg.get("import_then_solve"):
            return _discover_mixed_segments(cfg)
        if cfg.get("import_only"):
            # 纯导入段：合并为 1 个批次导入段（并发转换，imports_concurrency 生效）
            takes = discover_takes(cfg["footage_source_dir"])
            if not takes:
                progress.error("导入源目录下没有 take: {}".format(cfg["footage_source_dir"]))
                return []
            display_name = os.path.basename(cfg["footage_source_dir"].rstrip("/\\")) or "批次导入"
            return [{"type": "import", "name": display_name, "dir": cfg["footage_source_dir"],
                     "batch": "", "batch_import": True, "takes": takes}]
        pipeline.import_footage(cfg)
    return _find_footage_capture_data(cfg, cfg["mode"])


def _import_already_done(cfg: dict, item: dict) -> bool:
    """导入幂等：UE 侧该 take 的资产已存在即跳过。

    UE 目录名带 Slate/TakeNumber 后缀（磁盘 Enter_001 -> UE Enter_001_5，
    C++ PrepareAssetsData 的 SlateTake 命名），磁盘名是 UE 名的前缀——
    用资产包路径前缀匹配（base/{UE名}/CD_... 的第一段 startswith 磁盘名）。
    """
    root = cfg["capture_root"].rstrip("/")
    base = root if not item.get("batch") else "{}/{}".format(root, item["batch"])
    if not unreal.EditorAssetLibrary.does_directory_exist(base):
        return False
    try:
        # 匹配规则（实测两种形态，子串包含覆盖）：
        #   表演 take：UE 名 = 磁盘名 + TakeNumber 后缀（Enter_001 -> Enter_001_5）
        #   ROM take：磁盘名 = 日期前缀 + UE 名（20260710_face_am01_8 -> face_am01_8，
        #             UE 名是磁盘名的后缀段）
        for asset_path in unreal.EditorAssetLibrary.list_assets(base, recursive=True):
            pkg = asset_path.rsplit(".", 1)[0]
            tail = pkg[len(base):].strip("/")
            if not tail:
                continue
            first = tail.split("/", 1)[0]
            if item["name"] in first or first in item["name"]:
                return True
    except Exception:
        return False
    return False


def _match_footage_item(found: list, item_name: str):
    """解算段磁盘 take 名 <-> 已导入 CD 资产匹配（双向包含 + 最长 core 优先）。

    UE 侧 CD 名 = CD_{slate}_{take}（来自 take.json 元数据，非磁盘目录名），磁盘目录名
    （= take.json 的 identifier）实测三种形态：
    - 磁盘名 = slate（旧表演形态 Anims_xxx：磁盘名 ⊂ CD 名，正向包含）
    - 磁盘名 = {日期}_{slate}_{take}[_vN]（20260824_Anims_xxx_5_v2：CD core ⊂ 磁盘名，
      反向包含）；ROM 形态 20260710_face_am01_8 同为反向包含
    单向包含只覆盖第一种；此处双向取并集。多条命中时取 core 最长者（最精确优先，
    防 slate 数字结尾时短 core 误配长名，如 Ready_003_5 误配 Ready_003_5_1 的目录）。
    """
    best = None
    best_len = -1
    for f in found:
        name = f["name"]
        if not name.startswith("CD_"):
            continue
        core = name[len("CD_"):]
        if (item_name in name or core in item_name) and len(core) > best_len:
            best = f
            best_len = len(core)
    return best


def _discover_mixed_segments(cfg: dict) -> list:
    """混合段序列：导入段（每 take）+ 解算段（每 take）。段间 Exec 返回刷 UI——
    导入 37 分钟级与解算 34 分钟级全程共用 [i/N] 进度条。
    批次名来自 discover_takes 的层级自适应结果（填根目录/批次目录均可）。
    ROM take（目录名含批次名，如 face_am01 批下的 20260710_face_am01_8）自动排除——
    解算任务的语义是"表演 take -> FBX"，ROM 属于身份创建流程（面板①区），不参与。"""
    source = cfg.get("footage_source_dir", "")
    takes = discover_takes(source)
    if not takes:
        progress.error("导入源目录下没有 take: {}".format(source))
        return []
    perf_takes = []
    for name, take_dir, batch in takes:
        if batch and batch in name:
            progress.log("自动排除 ROM（不参与解算）: {}".format(name))
            continue
        perf_takes.append((name, take_dir, batch))
    if not perf_takes:
        progress.error("批次内没有表演 take（全部疑似 ROM？）: {}".format(source))
        return []
    # 导入段合并为 1 个批次导入段（batch_import=True）：段内并发导入全部 take（imports_concurrency），
    # 不再逐 take 占进度格；解算段仍逐 take（段间 Exec 返回刷 UI）。
    display_name = os.path.basename(source.rstrip("/\\")) or "批次导入"
    batch_segment = {
        "type": "import", "name": display_name, "dir": source, "batch": "", "batch_import": True,
        "takes": [(n, d, b) for n, d, b in perf_takes],
    }
    segments = [batch_segment]
    segments.extend({"type": cfg["mode"], "name": n, "batch": b} for n, _, b in perf_takes)
    progress.log("全自动任务展开: {} 个表演 take · 批次导入段 1 + 解算段 {}".format(
        len(perf_takes), len(perf_takes)))
    return segments


def _find_footage_capture_data(cfg: dict, mode: str):
    """递归扫描 capture_root，收集所有 FootageCaptureData 类资产。"""
    root = cfg["capture_root"]
    if not unreal.EditorAssetLibrary.does_directory_exist(root):
        progress.error("素材目录不存在: {}".format(root))
        return []

    items = []
    seen = set()
    for asset_path in unreal.EditorAssetLibrary.list_assets(root, recursive=True):
        try:
            pkg = asset_path.rsplit(".", 1)[0]
            if pkg in seen:
                continue
            asset = unreal.load_asset(pkg)
            if asset is None:
                continue
            if asset.get_class().get_name() == "FootageCaptureData":
                seen.add(pkg)
                items.append({
                    "type": mode,
                    "name": asset.get_name(),
                    "package_path": pkg,
                    # 批次级 identity 解析（identity_map 命中批次则映射，否则回落全局）
                    "identity": pipeline.resolve_identity(cfg, pkg),
                })
                progress.log("发现素材: {} (identity: {})".format(
                    asset.get_name(), items[-1]["identity"] or "<全局>"))
        except Exception as exc:
            progress.warn("跳过 {}: {}".format(asset_path, exc))
    # 显式按 name 排序，确保 run_next(index) 的 index 语义确定性（不依赖 list_assets 返回顺序）
    items.sort(key=lambda x: x["name"])
    return items


def _start_next():
    if _state["cancel"] or _state["index"] >= len(_state["items"]):
        _finish()
        return

    progress.status("running")
    # 批次上下文刷新（跨段持久）：控制台/progress.json 显示 [i/N] + 批次剩余预估
    progress.monitor_batch_context(
        _state["index"] + 1, len(_state["items"]), _remaining_seconds())
    item = _state["items"][_state["index"]]
    cfg = _state["cfg"]
    _state["shot_start"] = time.time()
    progress.log("开始处理 {}".format(item["name"]))

    try:
        if item["type"] == "audio":
            pipeline.run_audio_shot(cfg, item, _on_shot_done)
        elif item["type"] == "depth":
            pipeline.ensure_identity_ready(cfg, item.get("identity", ""))
            pipeline.run_depth_shot(cfg, item, _on_shot_done)
        else:  # video
            pipeline.ensure_identity_ready(cfg, item.get("identity", ""))
            pipeline.run_video_shot(cfg, item, _on_shot_done)
    except Exception as exc:
        _state["failed"].append(item["name"])
        _state["index"] += 1
        _mark_shot(item["name"], "failed")
        progress.error("{} 启动失败: {}".format(item["name"], exc))
        progress.monitor_end(False)  # 原生监控收尾（begin 后异常的兜底，幂等）
        _start_next()


def export_shot_outputs(cfg: dict, perf, item: dict):
    """导出 AS + 烘焙FBX + LS + ControlRig FBX（_on_shot_done 的导出核心）。

    批处理与流式执行器（stream_executor）共用；纯导出，不触碰批处理状态机。
    返回 (success: bool, error_msg: str)。
    """
    from . import export_fbx

    name = item["name"]
    try:
        asset_name = _asset_name(cfg, name)

        # 1) AnimSequence
        anim_seq = None
        if cfg.get("export_anim_sequence"):
            progress.monitor_stage(progress.STAGE_EXPORT_AS)
            anim_seq = pipeline.export_anim_sequence(perf, cfg, asset_name)
            if anim_seq is None:
                raise RuntimeError("AnimSequence 导出失败")

        # 2) 烘焙骨骼 FBX（音频模式唯一 FBX 产物；深度/视频默认不导）
        if cfg.get("export_baked_fbx") and anim_seq is not None:
            fbx_path = _fbx_export_path(cfg, item)
            if not export_fbx.export_anim_sequence_to_fbx(anim_seq, fbx_path):
                raise RuntimeError("烘焙 FBX 导出失败: {}".format(fbx_path))

        # 3) 深度/视频：Identity Level Sequence + ControlRig FACs FBX（交付标准）
        if item["type"] in ("depth", "video") and cfg.get("export_level_sequence"):
            progress.monitor_stage(progress.STAGE_EXPORT_LS)
            ls = pipeline.export_level_sequence(perf, cfg, asset_name)
            if ls is None:
                raise RuntimeError("Identity Level Sequence 导出失败")
            if cfg.get("export_fbx"):
                progress.monitor_stage(progress.STAGE_EXPORT_FBX)
                cr_fbx = _fbx_export_path(cfg, item)
                if not export_fbx.export_level_sequence_fbx_control_rig(ls, cr_fbx):
                    # 交付物缺失必须报失败，否则任务会标记 done 但 FBX 未产出（静默丢交付物）
                    raise RuntimeError("ControlRig FBX 导出失败（交付物缺失）: {}".format(cr_fbx))

        return True, ""
    except Exception as exc:
        progress.error("{} 导出失败: {}".format(name, exc))
        return False, str(exc)


def _on_shot_done(cfg: dict, perf, item: dict):
    """解算完成回调。导出 AS + LS + ControlRig FBX 后推进下一段。"""
    name = item["name"]
    success, error_msg = export_shot_outputs(cfg, perf, item)
    if success:
        _state["ok"] += 1
        progress.log("{} 完成".format(name))
    else:
        _state["failed"].append(name)

    # 记录耗时 + 更新状态 + 剩余预估
    elapsed = time.time() - _state["shot_start"]
    _state["timings"].append(elapsed)
    _mark_shot(name, "done" if success else "failed")
    # 每段结果（供 C++ 结果清单展示）：段名|成功|耗时
    progress.result(name, success, elapsed)
    # 交付清单：收集本段交付物（FBX 路径+大小 / AS / LS）
    _manifest_add(cfg, item, success, elapsed, error_msg)

    # 原生进度监控收尾：成功强制 100%（未激活时幂等无操作，音频/视频非阻塞路径安全）
    progress.monitor_end(success)

    _state["index"] += 1
    progress.progress(_state["index"], len(_state["items"]), name)
    eta = _estimate_remaining()
    if eta:
        progress.log("进度 {}/{} · 本段 {}s · 剩余预估 {}".format(
            _state["index"], len(_state["items"]), int(elapsed), eta))

    if _state.get("single_segment"):
        # 单段模式（C++ 定时器逐段驱动）：本段结束即返回，由 C++ 提交下一段。
        # 段间 Exec 返回，游戏线程空出，[MHS_PROGRESS]/[MHS_LOG] 日志得以被 UI 处理，实现段间刷新。
        progress.status("running" if not _state["failed"] else "error")
        _state["running"] = False
        _state["cancel"] = False
        progress.monitor_batch_context(0, 0, -1.0)  # 单段收尾：清空批次上下文
        _write_manifest()  # 交付清单覆盖写（本段完成后可立即查看）
        progress.done(_state["ok"], _state["failed"])
        return

    _start_next()


def _remaining_seconds() -> float:
    """批次剩余预估秒（基于已完段平均耗时 × 剩余段数）；无历史返回 -1。"""
    timings = _state["timings"]
    if not timings:
        return -1.0
    remaining = len(_state["items"]) - _state["index"]
    if remaining <= 0:
        return 0.0
    return sum(timings) / len(timings) * remaining


def _estimate_remaining() -> str:
    timings = _state["timings"]
    if not timings:
        return ""
    avg = sum(timings) / len(timings)
    remaining = len(_state["items"]) - _state["index"]
    secs = avg * remaining
    if secs < 60:
        return "{}s".format(int(secs))
    if secs < 3600:
        return "{}min".format(int(secs / 60))
    return "{:.1f}h".format(secs / 3600)


def _asset_name(cfg: dict, take_name: str) -> str:
    """生成素材名（目录键）——纯素材名，不带 prefix。

    混合模式剥日期前缀（_delivery_name）：UE 存储目录 /Game/MH_Results/{名} 用干净名；
    FBX 交付平铺在 {fbx_output_dir} 根下（无素材子目录），与 _fbx_export_path 的交付
    命名同源（幂等检查不漂移）。
    """
    from . import naming
    if cfg.get("import_then_solve"):
        take_name = _delivery_name(take_name)
    pattern = cfg["naming"].get("pattern", "{take_name}_{timecode}")
    return naming.build_name(take_name, "", pattern=pattern, prefix="")


def _finish():
    _state["running"] = False
    _state["cancel"] = False
    progress.monitor_batch_context(0, 0, -1.0)  # 批次结束：清空上下文
    if _state.get("manifest"):
        _write_manifest()  # 整批结束：交付清单最终版落盘
    progress.status("idle")
    progress.done(_state["ok"], _state["failed"])
