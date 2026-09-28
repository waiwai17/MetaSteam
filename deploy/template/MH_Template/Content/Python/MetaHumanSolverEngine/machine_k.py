# -*- coding: utf-8 -*-
"""帧数标定（S-B）：k = 秒/帧 —— 追平预估的换算系数

为什么需要：不同机器/素材的解算速度差异极大（i7-10700 实测 0.553 s/帧）。
用"已完任务实际耗时 ÷ 帧数"做滑动平均，随运行自动收敛到本机真实水平，
换机器无需改配置。

持久化：machine_k.json（与队列同目录），原子写；损坏/缺失回落 DEFAULT_K。
"""

import json
import os
import time

DEFAULT_K = 0.553          # 批次3 实测基准：1301s / 2351帧（i7-10700 + Preview 之外的档位）
MAX_SAMPLES = 5            # 滑动平均窗口（最近 N 条）


class MachineCalibration(object):
    """本机解算速度标定：秒/帧。"""

    def __init__(self, path, max_samples=MAX_SAMPLES):
        self.path = path
        self.max_samples = int(max_samples)
        self.samples = []      # [{frames, seconds, at}]
        self._load()

    def _load(self):
        if not self.path or not os.path.exists(self.path):
            return
        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            samples = data.get("samples")
            if isinstance(samples, list):
                self.samples = [s for s in samples
                                if isinstance(s, dict) and s.get("frames")
                                and s.get("seconds")][-self.max_samples:]
        except Exception:
            self.samples = []   # 损坏：回落默认 k，绝不阻断主流程

    def _save(self):
        if not self.path:
            return
        directory = os.path.dirname(self.path)
        if directory and not os.path.isdir(directory):
            try:
                os.makedirs(directory)
            except Exception:
                return
        payload = {"updated": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "k": self.k, "samples": self.samples}
        tmp_path = self.path + ".tmp"
        try:
            with open(tmp_path, "w", encoding="utf-8") as fh:
                json.dump(payload, fh, ensure_ascii=False)
            os.replace(tmp_path, self.path)
        except Exception:
            pass

    # ── 标定 ──
    def record(self, frames, seconds):
        """记录一条已完成任务的实测（帧数, 耗时秒）。无效输入忽略。"""
        try:
            frames = int(frames)
            seconds = float(seconds)
        except (TypeError, ValueError):
            return False
        if frames <= 0 or seconds <= 0:
            return False
        self.samples.append({"frames": frames, "seconds": round(seconds, 2),
                             "at": time.strftime("%H:%M")})
        if len(self.samples) > self.max_samples:
            del self.samples[:-self.max_samples]
        self._save()
        return True

    @property
    def k(self):
        """当前标定系数（秒/帧）：样本均值；无样本返回默认基准。"""
        if not self.samples:
            return DEFAULT_K
        total_f = sum(s["frames"] for s in self.samples)
        total_s = sum(s["seconds"] for s in self.samples)
        if total_f <= 0:
            return DEFAULT_K
        return round(total_s / total_f, 4)

    # ── 预估 ──
    def estimate_seconds(self, frames, default=None):
        """给定帧数预估解算秒数（无 k 时返回 default）。"""
        if not frames or frames <= 0:
            return default
        return round(frames * self.k)

    def avg_frames(self, fallback=None):
        """已完成样本的平均帧数（用于预估"未解算素材"的帧数）。"""
        if not self.samples:
            return fallback
        return int(sum(s["frames"] for s in self.samples) / len(self.samples))
