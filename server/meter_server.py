#!/usr/bin/env python3
"""額度顯示器的資料服務（2026-11-21）：給 LILYGO T-QT Pro 讀 http://<MBP>.local:8817/meter.json。

刻意不主動呼叫 agy：Antigravity 額度讀選單列小工具每 5 分鐘寫的 quota_cache.json，
避免多一個程式在斷網或登入失效時去刷新權杖（agy 守門的教訓，見 ANTIGRAVITY_規範）。
  - Antigravity 兩組（週／5 小時）：quota_cache.json，附資料時間
  - 生圖剩餘張數：agy_meter.image_quota()（本機帳本，不連網）
  - Claude Code 今日用量：claude_meter.summary()（掃描對話紀錄約 15 秒，10 分鐘算一次）
  - Colab 運算單元：colab_job.usage()（約 3 秒，30 分鐘一次）
只回應區網與本機位址。
"""
import ipaddress, json, os, subprocess, sys, threading, time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BRIDGE = Path(os.environ.get("AGY_BRIDGE", "/Users/Shared/antigravity-bridge"))   # antigravity-bridge 的位置
COLAB = Path(os.environ.get("COLAB_BRIDGE", "/Users/Shared/colab-bridge"))      # 沒有 Colab 時該欄顯示無資料
PORT = 8817
sys.path.insert(0, str(BRIDGE))

STATE = {"claude": None, "claude_at": 0, "colab": None, "colab_at": 0}
LOCK = threading.Lock()


def log(msg):
    print(time.strftime("%F %T"), msg, flush=True)


def epoch(iso):
    if not iso:
        return 0
    try:
        return int(datetime.fromisoformat(str(iso).replace("Z", "+00:00")).timestamp())
    except Exception:
        return 0


def antigravity():
    try:
        d = json.loads((BRIDGE / "quota_cache.json").read_text(encoding="utf-8"))
    except Exception as e:
        return {"ok": False, "error": f"讀不到快取：{type(e).__name__}"}
    groups = {"Gemini Models": "gemini", "Claude and GPT models": "third"}
    out = {"ok": True, "fetched": epoch(d.get("fetched_at"))}
    for b in d.get("buckets", []):
        g = groups.get(b.get("group"))
        if not g:
            continue
        key = f"{g}_{'wk' if b.get('window') == 'weekly' else '5h'}"
        out[key] = {"pct": round(float(b.get("remaining_fraction") or 0) * 100, 1),
                    "reset": epoch(b.get("reset_time"))}
    return out


def zone():
    try:
        import agy_guard
        b = agy_guard.blocked()
        if b:
            return {"blocked": True, "why": "學校網路，暫停查詢"}
        a = agy_guard.auth_locked()
        if a:
            return {"blocked": True, "why": "登入失效，請重新登入"}
    except Exception:
        pass
    return {"blocked": False, "why": ""}


def images():
    try:
        import importlib, agy_meter
        importlib.reload(agy_meter)
        q = agy_meter.image_quota()
        return {"ok": True, "left": q.get("estimated_remaining"), "limit": q.get("estimated_limit"),
                "exhausted": bool(q.get("exhausted")), "reset": epoch(q.get("window_reset"))}
    except Exception as e:
        return {"ok": False, "error": type(e).__name__}


def refresh_claude():
    try:
        import claude_meter
        now = datetime.now().astimezone()
        s = claude_meter.summary(since=now.replace(hour=0, minute=0, second=0, microsecond=0))
        tok = sum(v.get("billable_tokens", 0) for v in s["by_model"].values())
        out = sum(v.get("output_tokens", 0) for v in s["by_model"].values())
        res = {"ok": True, "calls": s.get("total_calls", 0), "billable": tok, "output": out}
    except Exception as e:
        res = {"ok": False, "error": type(e).__name__}
    with LOCK:
        STATE["claude"], STATE["claude_at"] = res, int(time.time())


def refresh_colab():
    try:
        p = subprocess.run([str(COLAB / "venv/bin/python"), "-c",
                            "import json,colab_job as c;u=c.usage();u.pop('raw',None);print(json.dumps(u))"],
                           cwd=COLAB, capture_output=True, text=True, timeout=90)
        u = json.loads(p.stdout.strip().splitlines()[-1])
        res = {"ok": bool(u.get("ok")), "balance": u.get("balance"), "rate": u.get("rate"), "active": u.get("active")}
    except Exception as e:
        res = {"ok": False, "error": type(e).__name__}
    with LOCK:
        STATE["colab"], STATE["colab_at"] = res, int(time.time())


def worker():
    while True:
        now = time.time()
        if now - STATE["claude_at"] > 600:
            refresh_claude()
        if now - STATE["colab_at"] > 1800:
            refresh_colab()
        time.sleep(30)


def payload():
    with LOCK:
        c, cl = STATE["claude"], STATE["colab"]
        cat, clat = STATE["claude_at"], STATE["colab_at"]
    return {"v": 1, "now": int(time.time()), "zone": zone(), "agy": antigravity(), "img": images(),
            "claude": dict(c or {"ok": False}, at=cat), "colab": dict(cl or {"ok": False}, at=clat)}


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            ip = ipaddress.ip_address(self.client_address[0])
            if not (ip.is_private or ip.is_loopback):
                self.send_error(403); return
        except ValueError:
            self.send_error(403); return
        if self.path.split("?")[0] not in ("/meter.json", "/"):
            self.send_error(404); return
        body = json.dumps(payload(), ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    threading.Thread(target=worker, daemon=True).start()
    log(f"額度服務啟動，埠 {PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), H).serve_forever()
