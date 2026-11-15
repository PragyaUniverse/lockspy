#!/usr/bin/env python3
"""Web UI for LockSpy (python standard library only).

Serves the page in ui/static and runs programs under LockSpy when the page asks.
Needs Linux; on a Mac `make ui` runs it inside the Docker container.

    python3 ui/server.py [--port 8080] [--host 127.0.0.1]

API
  GET  /api/programs          test programs from tests/expected.csv (+ build status)
  GET  /api/source?path=...   source file of a test program, for the code viewer
  GET  /api/bench             summary of the last benchmark (results/*.csv)
  POST /api/build             run `make`, return its output
  POST /api/run               {"program": "buggy/ab_ba", "args": "", "naive": false,
                               "watchdog_ms": 500, "mode": "run"|"hang"} -> report
  POST /api/check             {"code": "...C/C++ source...", "naive": false} -> compile it
                               with g++, run it under LockSpy, same answer as /api/run
"""
import argparse
import csv
import json
import os
import shlex
import subprocess
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATIC = os.path.join(ROOT, "ui", "static")
LIB = os.path.join(ROOT, "build", "libLockSpy.so")
KINDS = ["POTENTIAL_DEADLOCK", "LIVE_DEADLOCK", "RELOCK", "BAD_UNLOCK", "HELD_AT_EXIT"]
CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml"}


def describe(src_path):
    """First paragraph of the comment at the top of a test program (stops at usage lines)."""
    lines = []
    try:
        with open(src_path) as f:
            for line in f:
                if not line.startswith("//"):
                    break
                text = line[2:].strip()
                if not text or text.startswith("./") or text.lower().startswith("usage") or text[:2] in ("1.", " 1"):
                    break
                lines.append(text)
    except OSError:
        return ""
    text = " ".join(lines)
    if " - " in text:
        text = text.split(" - ", 1)[1]
    return text[:1].upper() + text[1:]


def programs():
    out = []
    with open(os.path.join(ROOT, "tests", "expected.csv")) as f:
        for row in csv.DictReader(f):
            prog, args = row["program"], row["args"]
            src = os.path.join("tests", prog + ".cpp")
            out.append({
                "id": (prog + " " + args).strip(),
                "program": prog, "args": args, "group": prog.split("/")[0],
                "expected": [] if row["expected"] == "NONE" else row["expected"].split(";"),
                "mode": row["mode"], "source": src,
                "built": os.path.exists(os.path.join(ROOT, "build", "tests", prog)),
                "description": describe(os.path.join(ROOT, src)),
            })
    return out


def inside_root(path):
    full = os.path.realpath(os.path.join(ROOT, path))
    return full if full.startswith(ROOT + os.sep) else None


def run_program(req):
    prog = str(req.get("program", ""))
    exe = inside_root(os.path.join("build", "tests", prog)) if "/" in prog and not prog.startswith("/") else None
    if prog.startswith("./") or prog.startswith("build/"):
        exe = inside_root(prog)
    if not exe or not os.path.isfile(exe):
        return 400, {"error": f"program not found: {prog} (build it with the Build button)"}
    if not os.path.exists(LIB):
        return 400, {"error": "build/libLockSpy.so is missing: press Build first"}
    return run_exe(exe, prog, shlex.split(str(req.get("args", ""))), req)


def run_exe(exe, name, args, req):
    """Run one binary with LockSpy preloaded and collect its JSON report."""
    mode = req.get("mode", "run")
    with tempfile.TemporaryDirectory(prefix="lockspy-ui-") as out:
        env = dict(os.environ,
                   LD_PRELOAD=LIB, LOCKSPY_FORMAT="json", LOCKSPY_OUTDIR=out,
                   LOCKSPY_WATCHDOG_MS=str(int(req.get("watchdog_ms", 500))),
                   LOCKSPY_ON_DEADLOCK="abort" if mode == "hang" else "report")
        if req.get("naive"):
            env["LOCKSPY_NAIVE"] = "1"
        t0 = time.time()
        try:
            p = subprocess.run([exe] + args, cwd=out, env=env, capture_output=True, text=True,
                               timeout=float(req.get("timeout", 20)), errors="replace")
            rc, stdout, stderr, timed_out = p.returncode, p.stdout, p.stderr, False
        except subprocess.TimeoutExpired as e:
            rc, timed_out = -9, True
            stdout = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            stderr = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
        seconds = time.time() - t0
        report = None
        path = os.path.join(out, "lockspy_report.json")
        if os.path.exists(path):
            with open(path) as f:
                report = json.load(f)
    found = sorted({f["type"] for f in (report or {}).get("findings", [])})
    return 200, {"program": name, "args": " ".join(args), "rc": rc, "timed_out": timed_out,
                 "aborted": rc == -6 or rc == 134, "seconds": round(seconds, 3),
                 "stdout": stdout[-20000:], "stderr": stderr[-60000:], "found": found, "report": report}


def check_code(req):
    """Compile code typed into the page and run it under LockSpy."""
    src = str(req.get("code", ""))
    if not src.strip():
        return 400, {"error": "write some code first"}
    if len(src) > 200_000:
        return 400, {"error": "that's too much code for the box (200 KB max)"}
    if not os.path.exists(LIB):
        return 400, {"error": "build/libLockSpy.so is missing: press Rebuild first"}
    with tempfile.TemporaryDirectory(prefix="lockspy-check-") as d:
        # Same flags as the test programs, so LockSpy can name files and lines.
        lang = "c" if req.get("lang") == "c" else "c++"
        path = os.path.join(d, "mycode.c" if lang == "c" else "mycode.cpp")
        with open(path, "w") as f:
            f.write(src)
        exe = os.path.join(d, "mycode")
        cmd = ["gcc", "-std=gnu11"] if lang == "c" else ["g++", "-std=c++17"]
        cmd += ["-g", "-O0", "-rdynamic", "-pthread", "-no-pie", "-Wall", path, "-o", exe]
        try:
            c = subprocess.run(cmd, cwd=d, capture_output=True, text=True, errors="replace", timeout=60)
        except subprocess.TimeoutExpired:
            return 200, {"compile_error": "the compiler took more than 60 seconds"}
        warnings = c.stderr.replace(d + os.sep, "")
        if c.returncode != 0:
            return 200, {"compile_error": warnings[-20000:]}
        req = dict(req, mode="hang", timeout=min(float(req.get("timeout", 15)), 30))
        code, out = run_exe(exe, "mycode", [], req)
        out["compiler"] = warnings[-20000:]
        return code, out


def bench():
    """Summary of the last `make bench` run (results/detection.csv and overhead.csv)."""
    tools = {}
    try:
        with open(os.path.join(ROOT, "results", "detection.csv")) as f:
            for r in csv.DictReader(f):
                t = tools.setdefault(r["tool"], {"caught": 0, "missed": 0, "false_alarms": 0, "clean": 0})
                key = {"caught": "caught", "missed": "missed", "false alarm": "false_alarms", "clean": "clean"}[r["verdict"]]
                t[key] += 1
        times = {}
        with open(os.path.join(ROOT, "results", "overhead.csv")) as f:
            for r in csv.DictReader(f):
                times.setdefault((r["program"], r["tool"]), []).append(float(r["seconds"]))
    except (OSError, KeyError):
        return {"available": False}
    med = {k: sorted(v)[len(v) // 2] for k, v in times.items()}
    for tool in ("lockspy", "tsan", "helgrind"):
        ratios = [med[(p, tool)] / med[(p, "bare")] for (p, t) in med if t == tool and (p, "bare") in med]
        if ratios and tool in tools:
            prod = 1.0
            for x in ratios:
                prod *= x
            tools[tool]["slowdown"] = round(prod ** (1 / len(ratios)), 1)
    return {"available": True, "tools": tools}


def build():
    p = subprocess.run(["make", "-j4"], cwd=ROOT, capture_output=True, text=True, errors="replace")
    return {"ok": p.returncode == 0, "output": (p.stdout + p.stderr)[-20000:]}


class Handler(BaseHTTPRequestHandler):
    server_version = "LockSpy/1.0"

    def log_message(self, fmt, *a):
        print("[ui]", self.address_string(), fmt % a, flush=True)

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/api/programs":
            return self.send_json(200, {"programs": programs(), "lib_built": os.path.exists(LIB)})
        if url.path == "/api/bench":
            return self.send_json(200, bench())
        if url.path == "/api/source":
            rel = parse_qs(url.query).get("path", [""])[0]
            full = inside_root(rel)
            if not full or not full.startswith(os.path.join(ROOT, "tests") + os.sep) or not os.path.isfile(full):
                return self.send_json(404, {"error": "no such source file"})
            with open(full) as f:
                return self.send_json(200, {"path": rel, "text": f.read()})
        name = "index.html" if url.path in ("/", "") else url.path.lstrip("/")
        full = os.path.realpath(os.path.join(STATIC, name))
        if not full.startswith(STATIC + os.sep) or not os.path.isfile(full):
            return self.send_json(404, {"error": "not found"})
        with open(full, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", CONTENT_TYPES.get(os.path.splitext(full)[1], "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            req = json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            return self.send_json(400, {"error": "request body is not JSON"})
        path = urlparse(self.path).path
        if path == "/api/run":
            code, obj = run_program(req)
            return self.send_json(code, obj)
        if path == "/api/check":
            code, obj = check_code(req)
            return self.send_json(code, obj)
        if path == "/api/build":
            return self.send_json(200, build())
        return self.send_json(404, {"error": "not found"})


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default=os.environ.get("LOCKSPY_UI_HOST", "127.0.0.1"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("LOCKSPY_UI_PORT", "8080")))
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"LockSpy UI: http://localhost:{a.port}  (Ctrl-C to stop)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
