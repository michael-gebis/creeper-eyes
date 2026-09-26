#!/usr/bin/env python3
"""Run the API suite against a board over and over, and catch it restarting.

    python tools/load_test.py --host frank.local --minutes 45 --port COM3 \\
        -- --eye-file dist/eyes/dragon.bin

What it is for: brownouts.  A board on a marginal supply browns out when its
current steps up, and the biggest step it takes is the render loop coming
back to full speed after a pause -- an eye-file upload, which stalls both
cores while flash is written, or the address cards.  The suite makes those
pauses over and over, so a supply that is close to its limit shows it within
the hour instead of once a week in a sealed head.  See docs/FRAME_RATE.md.

Two witnesses to a restart, because either alone can miss one:

- The board's own.  Between runs, an uptime that has fallen behind the clock
  means it restarted, and its warnings list then says why -- a brownout, a
  crash, a watchdog (health.h).
- The serial port, with --port.  The brownout detector's own message and the
  ROM's reset line, with the lines before them, and the heartbeat's fps.
  Opened without resetting the board.

Everything after `--` goes to test_api.py unchanged.  --eye-file is the one
that matters here: it is what makes the pauses.  Like test_api.py, it leaves
the file loaded in the slot afterwards.

Credentials come from include/secrets.h unless given, as tools/ota.py's do.
Exit status is 0 if the board never restarted and every run passed.
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import sys
import threading
import time
from typing import Any, Optional

HERE: str = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ota import from_secrets  # noqa: E402
from test_api import Api, Result  # noqa: E402

# Serial lines that mean the board went down.  "rst:" is the ROM's first
# line after any reset; the others say which kind.
ALARM: re.Pattern[str] = re.compile(
    r"Brownout|rst:0x|Guru Meditation|abort\(\)|Backtrace|task_wdt")
CONTEXT: int = 15  # lines of serial shown before each alarm

# Slack for the uptime check, in seconds: requests and rounding make the
# board's count and ours drift apart by a little without any restart.
UPTIME_SLACK: float = 10.0


class SerialWatch(threading.Thread):
    """Collects everything the board prints until stopped."""

    def __init__(self, port: str) -> None:
        super().__init__(daemon=True)
        import serial  # only needed with --port
        self.s: Any = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.5
        self.s.dtr = self.s.rts = False  # applied at open: no reset
        self.s.open()
        self.buf: bytes = b""
        self.done: threading.Event = threading.Event()

    def run(self) -> None:
        while not self.done.is_set():
            self.buf += self.s.read(4096)
        self.s.close()

    def lines(self) -> list[str]:
        return self.buf.decode("utf-8", "replace").splitlines()


def uptime(api: Api, wait: float = 90.0) -> Optional[int]:
    """The board's uptime in seconds, waiting out a restart in progress."""
    end: float = time.time() + wait
    while time.time() < end:
        try:
            return int(api.json("/state")["system"]["uptimeSeconds"])
        except (KeyError, TypeError, ValueError):
            time.sleep(3)  # api.json gives {} while the board is down
    return None


def restart_reason(api: Api) -> str:
    """What the board says it restarted after, from its warnings list."""
    try:
        found: list[str] = [w for w in api.json("/state")["system"]["warnings"]
                            if "restarted" in w]
    except (KeyError, TypeError):
        found = []
    return found[0] if found else "no reason given"


def suite(host: str, creds: list[str], extra: list[str]) -> tuple[bool, str]:
    """One run of test_api.py: whether it passed, and its summary line."""
    r: subprocess.CompletedProcess[str] = subprocess.run(
        [sys.executable, os.path.join(HERE, "test_api.py"), "--host", host]
        + creds + extra, capture_output=True, text=True)
    out: list[str] = (r.stdout + r.stderr).splitlines()
    summary: str = next((l.strip() for l in reversed(out) if "passed," in l),
                        "no summary (exit %d)" % r.returncode)
    fails: list[str] = [l.strip() for l in out if "[FAIL]" in l]
    return r.returncode == 0, " | ".join([summary] + fails)


def report_serial(lines: list[str]) -> None:
    """The fps, and each restart with what led up to it and the few lines
    after, which is where the ROM names the reset.  One block per restart."""
    fps: list[int] = [int(x) for x in re.findall(r"\] fps=(\d+)", "\n".join(lines))
                      if int(x)]  # 0 while a card is up or the slot is written
    print("serial: %d lines%s" % (len(lines), ", fps median %s, max %d"
                                   % (statistics.median(fps), max(fps)) if fps else ""))
    shown: int = -1
    for i, line in enumerate(lines):
        if ALARM.search(line) and i > shown:
            print("  ---")
            for c in lines[max(0, i - CONTEXT, shown + 1):i + 4]:
                print("      " + c)
            shown = i + 3


def main(argv: list[str]) -> int:
    extra: list[str] = []
    if "--" in argv:
        extra = argv[argv.index("--") + 1:]
        argv = argv[:argv.index("--")]
    ap: argparse.ArgumentParser = argparse.ArgumentParser(
        description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="frank.local")
    ap.add_argument("--minutes", type=float, default=45)
    ap.add_argument("--port", help="serial port to watch, e.g. COM3")
    ap.add_argument("--log", help="write the serial transcript here")
    ap.add_argument("--token", help="bearer token; read from secrets.h if omitted")
    ap.add_argument("--user", help="digest username; read from secrets.h if omitted")
    ap.add_argument("--password", help="digest password; read from secrets.h if omitted")
    args: argparse.Namespace = ap.parse_args(argv)

    token: Optional[str] = args.token or from_secrets("AUTH_TOKEN_VALUE")
    user: Optional[str] = args.user or from_secrets("AUTH_USER")
    password: Optional[str] = args.password or from_secrets("AUTH_PASS")
    creds: list[str] = []
    api: Api
    if token:
        creds = ["--token", token]
        api = Api(args.host, Result(), token=token)
    else:
        if user and password:
            creds = ["--user", user, "--password", password]
        api = Api(args.host, Result(), user, password)

    watch: Optional[SerialWatch] = SerialWatch(args.port) if args.port else None
    if watch:
        watch.start()

    start: float = time.time()
    deadline: float = start + args.minutes * 60
    last_up: Optional[int] = uptime(api)
    last_t: float = time.time()
    if last_up is None:
        print("%s does not answer" % args.host)
        return 1
    print("%s, up %ds; running the suite for %g minutes"
          % (args.host, last_up, args.minutes), flush=True)
    runs: int = 0
    failed: int = 0
    restarts: list[str] = []
    while time.time() < deadline:
        ok: bool
        summary: str
        ok, summary = suite(args.host, creds, extra)
        runs += 1
        failed += 0 if ok else 1
        up: Optional[int] = uptime(api)
        now: float = time.time()
        # Without a restart, uptime keeps pace with the clock.  Comparing it
        # with the last reading alone would miss a restart early in a long
        # run, whose new uptime can already exceed the old.
        note: str = ""
        if up is None:
            note = "  NOT ANSWERING"
            restarts.append("after run %d: stopped answering" % runs)
        elif up < last_up + (now - last_t) - UPTIME_SLACK:
            note = "  RESTARTED: " + restart_reason(api)
            restarts.append("during run %d, before %s: %s"
                            % (runs, time.strftime("%H:%M:%S"), restart_reason(api)))
        print("%s  run %d: %s%s" % (time.strftime("%H:%M:%S"), runs, summary, note),
              flush=True)
        if up is None:
            break
        last_up, last_t = up, now

    print("\n%.1f minutes, %d suite runs, %d with failures"
          % ((time.time() - start) / 60, runs, failed))
    print("restarts: %d" % len(restarts))
    for r in restarts:
        print("  " + r)

    if watch:
        watch.done.set()
        watch.join()
        lines: list[str] = watch.lines()
        if args.log:
            with open(args.log, "w", encoding="utf-8") as log:
                log.write("\n".join(lines))
        report_serial(lines)
    return 0 if not restarts and not failed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
