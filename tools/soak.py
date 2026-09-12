#!/usr/bin/env python3
"""Compare two firmware configurations over hours, fairly.

    python tools/soak.py --hours 3 --token ... \\
        --a "-DHTTP_TASK=1" --b "-DHTTP_TASK=0"

The problem this exists to solve: a WiFi link is not a stable measuring
instrument.  Measuring configuration A for ten minutes and then B for ten
minutes compares A's radio conditions against B's, and over one afternoon
that was enough to invert a conclusion completely -- the render-loop build
and the task build each looked catastrophic when measured while the air was
bad, and fine when measured while it was good.

So this alternates.  Each round builds, flashes and measures both, one after
the other, and reverses the order every round so that any drift within a
round cancels across rounds rather than accumulating into one side.

Everything is written to a CSV as it goes, so a run that is interrupted after
two hours is still two hours of data, and the analysis can be redone without
re-measuring.

Requires tools/test_api.py beside it, for the transport and the statistics.
"""

from __future__ import annotations

import argparse
import csv
import os
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_api import Api, Result, percentile  # noqa: E402

# A request slower than this is the thing being counted: not a slow reply but
# a stalled one, an order of magnitude off the median.
STALL_MS = 500.0


class Config:
    """One side of the comparison."""

    def __init__(self, name: str, flags: str) -> None:
        self.name: str = name
        self.flags: str = flags
        self.samples: list[float] = []
        self.stalls: int = 0
        self.failures: int = 0
        self.rounds: int = 0


def run(cmd: list[str], env: Optional[dict[str, str]] = None,
        timeout: int = 600) -> tuple[int, str]:
    full = dict(os.environ)
    if env:
        full.update(env)
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           env=full, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return 1, "timed out"


def flash(pio: str, env_name: str, port: str, flags: str,
          build_dir: str) -> bool:
    code, out = run([pio, "run", "-e", env_name, "-t", "upload",
                     "--upload-port", port],
                    {"PLATFORMIO_BUILD_FLAGS": flags,
                     "PLATFORMIO_BUILD_DIR": build_dir})
    if "SUCCESS" in out and code == 0:
        return True
    tail = [l for l in out.splitlines() if "error" in l.lower()][:3]
    print("      flash failed: %s" % ("; ".join(tail) or "unknown"))
    return False


def wait_for_board(host: str, limit: int = 90) -> bool:
    """Up and answering, whether or not it wants a credential."""
    deadline = time.time() + limit
    while time.time() < deadline:
        try:
            urllib.request.urlopen("http://%s/" % host, timeout=4)
            return True
        except urllib.error.HTTPError:
            return True  # 401 is an answer
        except Exception:
            time.sleep(3)
    return False


def measure(api: Api, n: int, gap: float) -> tuple[list[float], int]:
    """n identical requests.  Returns the timings and a failure count."""
    out: list[float] = []
    failed = 0
    for _ in range(n):
        t0 = time.time()
        code, _body = api.raw("/state")
        if code == 200:
            out.append((time.time() - t0) * 1000.0)
        else:
            failed += 1
        time.sleep(gap)
    return out, failed


def summarise(c: Config) -> str:
    if not c.samples:
        return "%-10s no samples" % c.name
    s = sorted(c.samples)
    return ("%-10s n=%-5d median %5.0f  p90 %5.0f  p99 %6.0f  max %7.0f  "
            "stalls %3d (%.2f%%)  failed %d"
            % (c.name, len(s), percentile(s, 50), percentile(s, 90),
               percentile(s, 99), s[-1], c.stalls,
               100.0 * c.stalls / len(s), c.failures))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hours", type=float, default=2.0)
    ap.add_argument("--host", default="frank.local")
    ap.add_argument("--port", default="COM3", help="serial port for flashing")
    ap.add_argument("--env", default="gray_rtc", help="platformio environment")
    ap.add_argument("--a", default="-DHTTP_TASK=1", help="build flags for A")
    ap.add_argument("--b", default="-DHTTP_TASK=0", help="build flags for B")
    ap.add_argument("--a-name", default="task")
    ap.add_argument("--b-name", default="loop")
    ap.add_argument("--samples", type=int, default=60,
                    help="requests per configuration per round")
    ap.add_argument("--gap", type=float, default=0.2,
                    help="seconds between requests")
    ap.add_argument("--token")
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--csv", default="soak.csv")
    ap.add_argument("--pio", default=os.path.expanduser(
        "~/.platformio/penv/Scripts/pio.exe"))
    ap.add_argument("--build-dir", default="")
    args = ap.parse_args(argv[1:])

    a = Config(args.a_name, args.a)
    b = Config(args.b_name, args.b)
    res = Result()
    api = Api(args.host, res, args.user, args.password, args.token, 20.0)

    deadline = time.time() + args.hours * 3600
    order: list[Config] = [a, b]
    rnd = 0

    fresh = not os.path.exists(args.csv)
    fh = open(args.csv, "a", newline="", encoding="utf-8")
    out = csv.writer(fh)
    if fresh:
        out.writerow(["round", "config", "flags", "unix_time", "ms"])

    print("soaking for %.1f h: %s (%s) against %s (%s)"
          % (args.hours, a.name, a.flags, b.name, b.flags))
    print("%d samples each per round, alternating order, writing %s\n"
          % (args.samples, args.csv))

    try:
        while time.time() < deadline:
            rnd += 1
            left = (deadline - time.time()) / 3600.0
            print("round %d  (%.2f h left)" % (rnd, left))
            for cfg in order:
                print("   %-6s flashing..." % cfg.name, end="", flush=True)
                if not flash(args.pio, args.env, args.port, cfg.flags,
                             args.build_dir or os.environ.get(
                                 "PLATFORMIO_BUILD_DIR", ".pio/build")):
                    print()
                    continue
                if not wait_for_board(args.host):
                    print(" board did not come back")
                    continue
                time.sleep(2)
                got, failed = measure(api, args.samples, args.gap)
                stalls = len([v for v in got if v > STALL_MS])
                cfg.samples += got
                cfg.stalls += stalls
                cfg.failures += failed
                cfg.rounds += 1
                now = time.time()
                for v in got:
                    out.writerow([rnd, cfg.name, cfg.flags, "%.3f" % now,
                                  "%.1f" % v])
                fh.flush()
                s = sorted(got)
                print("\r   %-6s median %5.0f  p99 %6.0f  max %7.0f  "
                      "stalls %d/%d          "
                      % (cfg.name, percentile(s, 50) if s else 0,
                         percentile(s, 99) if s else 0, s[-1] if s else 0,
                         stalls, len(got)))
            order.reverse()  # so drift inside a round cancels across rounds
            print()
    except KeyboardInterrupt:
        print("\nstopped early -- what was measured is still in %s\n" % args.csv)

    fh.close()
    print("=" * 78)
    print("after %d rounds" % rnd)
    print("  " + summarise(a))
    print("  " + summarise(b))

    if a.samples and b.samples:
        print()
        # Stalls are the number that decides this; a median that improves
        # while the tail collapses is not an improvement.
        ra = 100.0 * a.stalls / len(a.samples)
        rb = 100.0 * b.stalls / len(b.samples)
        better = a.name if ra < rb else b.name if rb < ra else "neither"
        print("  stalls over %.0f ms: %s %.2f%%, %s %.2f%%  -> %s"
              % (STALL_MS, a.name, ra, b.name, rb, better))
        ma, mb = statistics.median(a.samples), statistics.median(b.samples)
        print("  median:            %s %.0f ms, %s %.0f ms  -> %s by %.0f ms"
              % (a.name, ma, b.name, mb,
                 a.name if ma < mb else b.name, abs(ma - mb)))
        print()
        print("  Both sides saw the same hours of radio, which is the whole")
        print("  point: a difference that survives that is a difference in")
        print("  the firmware.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
