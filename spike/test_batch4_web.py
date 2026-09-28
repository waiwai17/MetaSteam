# -*- coding: utf-8 -*-
"""批次4 · 层3 端到端 HTTP 测试：StreamWebServer（局域网手机可见的入口）

覆盖：三端点内容与类型 / 404 / 405 / 幂等启动 / 0.0.0.0 绑定（非 loopback 可达）/
      并发读 / stop
"""

import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import urllib.request
import urllib.error

PKG_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(PKG_ROOT, "deploy", "Python"))

from MetaHumanSolverEngine.stream_state import StreamWebServer

OK = [True]
LINES = []


def check(name, cond, detail=""):
    line = "[{}] {}{}".format("PASS" if cond else "FAIL", name,
                              ("  -> " + str(detail)) if (detail and not cond) else "")
    print(line)
    LINES.append(line)
    if not cond:
        OK[0] = False


def http(method, url):
    """返回 (status, body_bytes, content_type)"""
    req = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, resp.read(), resp.headers.get("Content-Type", "")
    except urllib.error.HTTPError as exc:
        return exc.code, b"", ""


def main():
    tmp = tempfile.mkdtemp(prefix="mhs_w_")
    try:
        # 准备 json_dir：真实形态的 stream_state.json + progress.json
        json_dir = os.path.join(tmp, "jd")
        os.makedirs(json_dir)
        with open(os.path.join(json_dir, "stream_state.json"), "w", encoding="utf-8") as fh:
            json.dump({"state": "idle", "delivered": 1, "synced": 1, "failed": 0,
                       "queued": 0, "stage_counts": [0, 1, 1, 1],
                       "identity_groups": [{"dir": "pm/", "id": "ID_pm", "until": "",
                                            "now": False, "total": 1, "done": 1,
                                            "failed": 0, "pending": 0}],
                       "events": [], "eta_seconds": None,
                       "updated_at": "2026-09-18 15:00:00"}, fh, ensure_ascii=False)
        with open(os.path.join(json_dir, "progress.json"), "w", encoding="utf-8") as fh:
            json.dump({"state": "idle", "updated_at": "2026-09-18 15:00:00"}, fh)

        # 随机高位端口
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        base = "http://127.0.0.1:{}".format(port)

        # ── 启动 ──
        check("1 启动成功", StreamWebServer.start(port, json_dir) is True)
        check("2 同端口幂等启动（跳过）", StreamWebServer.start(port, json_dir) is True)
        check("3 不同端口拒绝（实例已占用）", StreamWebServer.start(port + 1, json_dir) is False)

        # ── 三端点 ──
        st, body, ctype = http("GET", base + "/")
        check("4 GET / -> 200 html", st == 200 and "text/html" in ctype, (st, ctype))
        check("5 dashboard 新版实时面板（大数字/进度/模块/事件/ETA）",
              b"Stream" in body and b"bigNum" in body and b"pCard" in body and b"events" in body
              and b"eta_seconds" in body and b"stream_state.json" in body)

        st, body, ctype = http("GET", base + "/stream_state.json")
        ok_json = False
        try:
            data = json.loads(body.decode("utf-8"))
            ok_json = data.get("delivered") == 1 and "stage_counts" in data
        except Exception:
            pass
        check("6 GET /stream_state.json -> 200 可解析契约数据",
              st == 200 and "application/json" in ctype and ok_json, st)

        st, body, ctype = http("GET", base + "/progress.json")
        ok_p = False
        try:
            ok_p = json.loads(body.decode("utf-8")).get("state") == "idle"
        except Exception:
            pass
        check("7 GET /progress.json -> 200 可解析", st == 200 and ok_p, st)

        # ── 拒绝面 ──
        st, _, _ = http("GET", base + "/nope")
        check("8 未知路径 -> 404", st == 404, st)
        st, _, _ = http("GET", base + "/../stream_queue_test.json")
        check("9 目录穿越尝试 -> 404（白名单路由）", st == 404, st)
        st, _, _ = http("POST", base + "/")
        check("10 POST -> 405（只读服务）", st == 405, st)

        # ── 0.0.0.0 绑定（手机可达的前提）──
        local_ip = ""
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect(("8.8.8.8", 80))     # 不实际发包，仅取路由出口 IP
                local_ip = s.getsockname()[0]
        except Exception:
            pass
        if local_ip and local_ip != "127.0.0.1":
            st, _, _ = http("GET", "http://{}:{}/".format(local_ip, port))
            check("11 非 loopback IP 可达（0.0.0.0 绑定生效: {}）".format(local_ip), st == 200, st)
        else:
            print("[SKIP] 11 无独立局域网 IP（单机环境）")

        # ── 并发读 ──
        errors = []

        def hammer(n):
            for _ in range(n):
                try:
                    st, body, _ = http("GET", base + "/stream_state.json")
                    if st != 200:
                        errors.append(st)
                    else:
                        json.loads(body.decode("utf-8"))
                except Exception as exc:
                    errors.append(str(exc))

        threads = [threading.Thread(target=hammer, args=(15,)) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        check("12 并发 8x15 读零失败", not errors, errors[:3])

        # ── stop ──
        StreamWebServer.stop()
        time_wait = 0
        import time as _t
        while time_wait < 3:
            try:
                http("GET", base + "/")
                _t.sleep(0.2)
                time_wait += 0.2
            except Exception:
                break
        closed = False
        try:
            http("GET", base + "/")
        except Exception:
            closed = True
        check("13 stop 后端口关闭", closed)
        check("14 stop 后可重新启动", StreamWebServer.start(port, json_dir) is True)
        StreamWebServer.stop()
    finally:
        try:
            StreamWebServer.stop()
        except Exception:
            pass
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    print("=== 批次4 Web 测试: " + ("全部通过" if OK[0] else "存在失败") + " ===")
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "batch4_web_result.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(LINES) + "\n")
    return 0 if OK[0] else 1


if __name__ == "__main__":
    sys.exit(main())
