#!/usr/bin/env python3
"""Exercise a running board's REST API.

    uv run tools/test_api.py --host frank.local
    uv run tools/test_api.py --host 192.168.1.50 --user frank --password ...
    uv run tools/test_api.py --host frank.local --token ...

Runs against real hardware, because that is the only place the interesting
failures live: a handler that works in isolation but starves the render loop,
a value that survives a round trip but not a reboot, a verb that returns the
wrong status only when the body is malformed.

It adapts to the firmware it finds.  A build without an RTC, without the
clock, or without authentication simply has those groups skipped rather than
failed -- `GET /api/v1/info` says which features are compiled in, and the
suite believes it.

Nothing here reboots the board or writes to flash unless you ask: --wifi
covers the credential endpoints, which reboot; --settings covers save and
forget, which write NVS; and --restart restarts the board twice, moving the
CPU speed setting across a restart and back.  None runs by default.  Nor does
--eye-file, which fills the eye slot; the uploads the board refuses before
erasing anything are tested every time.

The board's own state is captured at the start and put back at the end, so a
run leaves the eyes as it found them.

Exit status is 0 if everything passed or was skipped, 1 otherwise.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import socket
import struct
import sys
import time
import urllib.error
import urllib.request
from typing import Any

import make_eye  # beside this file; builds the eye files the slot tests send

Json = dict[str, Any]


# --------------------------------------------------------------------- state --

class Result:
    """Tallies, and the detail needed to explain a failure afterwards."""

    def __init__(self) -> None:
        self.passed: int = 0
        self.failed: list[str] = []
        self.skipped: list[str] = []
        self.group: str = ""
        self.slowest: float = 0.0
        self.slowest_what: str = ""
        self.slow: list[tuple[float, str]] = []
        self.samples: list[tuple[float, str]] = []
        self.requests: int = 0
        self.total_ms: float = 0.0

    def ok(self, label: str, detail: str = "") -> None:
        self.passed += 1
        print("   [ok]   %-52s %s" % (label, detail))

    def fail(self, label: str, detail: str) -> None:
        self.failed.append("%s / %s: %s" % (self.group, label, detail))
        print("   [FAIL] %-52s %s" % (label, detail))

    def skip(self, label: str, why: str) -> None:
        self.skipped.append("%s / %s" % (self.group, label))
        print("   [skip] %-52s %s" % (label, why))

    def heading(self, name: str) -> None:
        self.group = name
        print("\n== %s ==" % name)


# ------------------------------------------------------------------ transport --

class Api:
    """The device, over HTTP.  Carries whichever credential was configured."""

    def __init__(self, host: str, res: Result, user: str | None = None,
                 password: str | None = None, token: str | None = None,
                 timeout: float = 10.0) -> None:
        self.base: str = "http://%s/api/v1" % host
        self.root: str = "http://%s" % host
        self.res: Result = res
        self.token: str | None = token
        self.timeout: float = timeout
        self.user: str | None = user
        self.password: str | None = password
        self.opener: urllib.request.OpenerDirector
        self.recredential(user, password)

    def recredential(self, user: str | None,
                     password: str | None) -> None:
        """Start using a different password.

        Needed because the credential test changes the board's password and
        has to keep talking to it afterwards.  The opener caches the digest
        credentials it was built with, so a new password means a new opener --
        assigning to self.password alone would change nothing.
        """
        self.user = user
        self.password = password
        if user and password:
            # Digest, matching AUTH_HTTP.  urllib retries with credentials
            # after the challenge, exactly as a browser does.
            mgr: urllib.request.HTTPPasswordMgrWithDefaultRealm = \
                urllib.request.HTTPPasswordMgrWithDefaultRealm()
            mgr.add_password(None, self.root, user, password)
            self.digest = urllib.request.HTTPDigestAuthHandler(mgr)
            self.opener = urllib.request.build_opener(self.digest)
        else:
            self.digest = None
            self.opener = urllib.request.build_opener()

    def raw(self, path: str, method: str = "GET",
            body: Any | None = None,
            headers: dict[str, str] | None = None,
            full: bool = False) -> tuple[int, bytes]:
        """One request.  Returns the status and body; never raises for HTTP.
        A bytes body is sent as it is, the way an eye file is uploaded."""
        url: str = (self.root + path) if full else (self.base + path)
        req: urllib.request.Request = urllib.request.Request(
            url, method=method)
        data: bytes | None = None
        if isinstance(body, bytes):
            data = body
            req.add_header("Content-Type", "application/octet-stream")
        elif body is not None:
            data = json.dumps(body).encode()
            req.add_header("Content-Type", "application/json")
        if self.token:
            req.add_header("Authorization", "Bearer " + self.token)
        for k, v in (headers or {}).items():
            req.add_header(k, v)

        started: float = time.time()
        # The board answers one client at a time from inside its render loop,
        # so an occasional request loses a race with a long frame.  Retrying a
        # read-only request is honest; a retried write would not be.
        attempts: int = 3 if method == "GET" else 1
        for attempt in range(attempts):
            # urllib's digest handler counts retries and gives up with
            # "digest auth failed" after five -- but only a success resets the
            # count, so five authenticated 4xx replies in a row (which the
            # refusal tests are made of) would fail the sixth.  Each request
            # here is its own exchange; start its count from zero.
            if self.digest:
                self.digest.reset_retry_count()
            try:
                with self.opener.open(req, data, timeout=self.timeout) as r:
                    out: tuple[int, bytes] = (r.status, r.read())
                    break
            except urllib.error.HTTPError as e:
                # Not every HTTPError has a body: the ones urllib raises
                # itself, like that digest give-up, have nothing to read.
                try:
                    reply: bytes = e.read()
                except Exception:  # noqa: BLE001
                    reply = b""
                out = (e.code, reply)
                break
            except Exception as e:  # noqa: BLE001 -- timeout, reset, DNS
                if attempt == attempts - 1:
                    self.res.fail("%s %s" % (method, path), repr(e)[:70])
                    return (0, b"")
                time.sleep(1.0)
        ms: float = (time.time() - started) * 1000
        self.res.requests += 1
        self.res.total_ms += ms
        what: str = "%s %s" % (method, path)
        self.res.samples.append((ms, what))
        if ms > self.res.slowest:
            self.res.slowest = ms
            self.res.slowest_what = what
        if ms > 1000:
            self.res.slow.append((ms, what))
        return out

    def json(self, path: str, method: str = "GET",
             body: Json | None = None) -> Json:
        code, raw = self.raw(path, method, body)
        if code == 0:
            return {}
        try:
            return json.loads(raw)
        except ValueError:
            return {}


# ---------------------------------------------------------------- assertions --

def expect(res: Result, api: Api, label: str, path: str, method: str = "GET",
           body: Any | None = None, status: int = 200,
           headers: dict[str, str] | None = None,
           full: bool = False) -> Json:
    """A request whose status is the thing under test."""
    code, raw = api.raw(path, method, body, headers, full)
    if code != status:
        res.fail(label, "got %s, wanted %s  %s" % (code, status, raw[:60]))
        return {}
    res.ok(label, "%s" % status)
    try:
        return json.loads(raw)
    except ValueError:
        return {}


def field(res: Result, obj: Json, path: str, label: str = "") -> Any:
    """Walk a dotted path, failing rather than raising if it is absent."""
    cur: Any = obj
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            res.fail(label or path, "missing field %s" % path)
            return None
        cur = cur[part]
    return cur


def same(res: Result, label: str, got: Any, want: Any) -> bool:
    if got == want:
        res.ok(label, repr(got)[:40])
        return True
    res.fail(label, "got %r, wanted %r" % (got, want))
    return False


# --------------------------------------------------------------------- groups --

def test_info(res: Result, api: Api) -> Json:
    res.heading("what this firmware is")
    info: Json = expect(res, api, "GET /info", "/info")
    for key in ("name", "version", "commit", "project", "built", "api"):
        if key not in info:
            res.fail("/info has %s" % key, "absent")
    if info.get("api") == "v1":
        res.ok("api version", "v1")
    elif info:
        res.fail("api version", "got %r" % info.get("api"))
    if info.get("dirty"):
        res.ok("build is marked dirty", "built from uncommitted changes")
    print("   firmware %s (%s), rtc=%s auth=%s"
          % (info.get("version"), info.get("commit"),
             info.get("rtc"), info.get("auth")))
    return info


def test_state(res: Result, api: Api) -> Json:
    res.heading("GET /state -- everything a client polls")
    s: Json = expect(res, api, "GET /state", "/state")
    before: int = len(res.failed)
    count: int = 0
    for path in ("eye.index", "eye.name", "eye.count",
                 "gaze.mode", "gaze.x", "gaze.y",
                 "dilate.mode", "dilate.percent",
                 "pupil.on", "swap.on", "flip.left", "flip.right",
                 "dim.percent", "dim.shown", "dim.gamma", "dim.trim.left",
                 "dim.trim.right", "dim.sweeping",
                 "startle.active",
                 "clock.on", "clock.seconds", "clock.time", "clock.rate",
                 "clock.colors.hour", "clock.colors.minute",
                 "clock.colors.second", "clock.suppressed",
                 "net.mac", "net.mdns", "net.state", "net.tz",
                 "time.source", "time.ntp.enabled", "time.rtc.enabled",
                 "system.fps", "system.panel", "system.panels",
                 "system.freeHeap", "system.uptimeSeconds",
                 "system.settingsDirty"):
        field(res, s, path, "state has %s" % path)
        count += 1
    if s and len(res.failed) == before:
        res.ok("all %d documented fields present" % count)
    return s


def test_eyes(res: Result, api: Api) -> None:
    res.heading("eyes")
    designs: list[Any] = expect(
        res, api, "GET /eyes", "/eyes").get("designs", [])
    if not designs:
        res.fail("eye list", "empty")
        return
    res.ok("designs built in", "%d: %s" % (
        len(designs), ", ".join(d["name"] for d in designs[:6])))

    first, last = designs[0], designs[-1]
    r: Json = expect(res, api, "PUT /eye by name", "/eye", "PUT",
               {"name": last["name"]})
    same(res, "selected by name", r.get("name"), last["name"])

    r = expect(res, api, "PUT /eye by index", "/eye", "PUT",
               {"index": first["index"]})
    same(res, "selected by index", r.get("index"), first["index"])

    before: Any = api.json("/eye").get("index")
    r = expect(res, api, "PUT /eye next", "/eye", "PUT", {"next": True})
    if len(designs) > 1:
        if r.get("index") != before:
            res.ok("next moved on", "%s -> %s" % (before, r.get("index")))
        else:
            res.fail("next moved on", "stayed at %s" % before)
    else:
        res.skip("next moved on", "only one design built in")

    expect(res, api, "unknown name is 404", "/eye", "PUT",
           {"name": "nosucheye"}, status=404)
    expect(res, api, "index past the end is 404", "/eye", "PUT",
           {"index": 999}, status=404)
    expect(res, api, "empty body is 400", "/eye", "PUT", {}, status=400)


def test_validation(res: Result, api: Api, start: Json) -> None:
    """Input the API must refuse, and refuse whole.  Each of these was once
    accepted as something else: a range check made after a narrowing cast
    let 300 through as 44, sscanf read "12:30junk" as 12:30, and a field of
    the wrong type was skipped while the request reported success."""
    res.heading("refusing bad input")
    field(res, start, "system.warnings", "state reports warnings")

    for label, body in (("a JSON array", [1, 2]), ("a bare number", 5)):
        expect(res, api, "%s is not a body" % label, "/pupil", "PUT", body,
               status=400)

    expect(res, api, "percent 300 is 400", "/dilate", "PUT",
           {"percent": 300}, status=400)
    expect(res, api, "percent -1 is 400", "/dilate", "PUT",
           {"percent": -1}, status=400)
    expect(res, api, "percent \"50\" is 400", "/dilate", "PUT",
           {"percent": "50"}, status=400)
    expect(res, api, "index 256 is 404", "/eye", "PUT", {"index": 256},
           status=404)
    expect(res, api, "gaze 65636 is 400", "/gaze", "PUT",
           {"x": 65636, "y": 100}, status=400)
    expect(res, api, "a nonsense zone is 400", "/tz", "PUT",
           {"tz": "hello world"}, status=400)

    flip: Json = api.json("/flip")
    expect(res, api, "flip with one bad side is 400", "/flip", "PUT",
           {"left": not flip.get("left"), "right": "yes"}, status=400)
    same(res, "and the good side did not change",
         api.json("/flip").get("left"), flip.get("left"))

    if start.get("clock"):
        before: Json = api.json("/clock")
        for label, body in (
                ("rate 65537", {"rate": 65537}),
                ("rate \"5\"", {"rate": "5"}),
                ("time 257:00", {"time": "257:00"}),
                ("time 12:30junk", {"time": "12:30junk"}),
                ("colour \"\"", {"colors": {"hour": ""}}),
                ("colour FF88", {"colors": {"hour": "FF88"}}),
                ("colour +FF8800", {"colors": {"hour": "+FF8800"}}),
                ("colors as a string", {"colors": "red"})):
            expect(res, api, "%s is 400" % label, "/clock", "PUT", body,
                   status=400)
        expect(res, api, "a good rate beside a bad colour is 400", "/clock",
               "PUT", {"rate": 2, "colors": {"hour": "zz"}}, status=400)
        same(res, "and the rate did not change",
             api.json("/clock").get("rate"), before.get("rate"))
    else:
        res.skip("clock input", "no clock in this build")

    if start.get("sleep"):
        for label, body in (("start 25:00", {"start": "25:00"}),
                            ("start 22:00x", {"start": "22:00x"}),
                            ("level 101", {"level": 101}),
                            ("enabled \"yes\"", {"enabled": "yes"})):
            expect(res, api, "sleep %s is 400" % label, "/sleep", "PUT", body,
                   status=400)
    else:
        res.skip("sleep input", "no sleep mode in this build")

    # Every one of these would reboot the board if it were accepted, which
    # is exactly why each must not be.
    for label, body in (("an SSID with a control character",
                         {"ssid": "home\u0001net"}),
                        ("a password that is a number",
                         {"ssid": "home", "pass": 12345678}),
                        ("a 33-character SSID", {"ssid": "x" * 33})):
        expect(res, api, "wifi: %s is 400" % label, "/wifi", "PUT", body,
               status=400)


def settle_shown(api: Api, want: int, within: float = 2.0) -> Any:
    """Poll dim.shown until it reaches `want` or time runs out; a fade takes
    DIM_FADE_MS for the whole range, so this allows it several times over.

    Only a reading asked for after the deadline may be the last word.  A reply
    can arrive seconds after the board wrote it, on a slow link, and one
    reading taken as the fade began and delivered after the deadline used to
    fail the test with the fade half done."""
    deadline: float = time.time() + within
    while True:
        asked: float = time.time()
        got: Any = api.json("/dim").get("shown")
        if got == want or asked >= deadline:
            return got
        time.sleep(0.2)


def test_dim(res: Result, api: Api, start: Json) -> None:
    """The dimmer.  Nothing here is saved; the board is put back after."""
    res.heading("brightness")
    before: Json = expect(res, api, "GET /dim", "/dim")
    if not before:
        return

    r: Json = expect(res, api, "PUT /dim percent 40", "/dim", "PUT", {"percent": 40})
    same(res, "set to 40", r.get("percent"), 40)
    same(res, "fades to 40", settle_shown(api, 40), 40)
    expect(res, api, "PUT /dim percent 0", "/dim", "PUT", {"percent": 0})
    same(res, "0 fades all the way off", settle_shown(api, 0), 0)
    fps: Any = api.json("/state").get("system", {}).get("fps")
    res.ok("still answering while off", "fps %s" % fps)

    r = expect(res, api, "PUT /dim gamma 1.8", "/dim", "PUT", {"gamma": 1.8})
    same(res, "gamma reads back", r.get("gamma"), 1.8)
    r = expect(res, api, "trim left alone", "/dim", "PUT", {"trim": {"left": 10}})
    same(res, "left trimmed, right untouched",
         (r.get("trim", {}).get("left"), r.get("trim", {}).get("right")),
         (10, before.get("trim", {}).get("right")))

    for label, body in (("percent 101", {"percent": 101}),
                        ("percent \"50\"", {"percent": "50"}),
                        ("gamma 0.5", {"gamma": 0.5}),
                        ("gamma 4.1", {"gamma": 4.1}),
                        ("gamma \"2.2\"", {"gamma": "2.2"}),
                        ("trim 51", {"trim": {"left": 51}}),
                        ("trim as a number", {"trim": 5}),
                        ("sweep \"yes\"", {"sweep": "yes"}),
                        ("an empty change", {})):
        expect(res, api, "%s is 400" % label, "/dim", "PUT", body, status=400)
    now: Json = api.json("/dim")
    expect(res, api, "a good percent beside a bad trim is 400", "/dim", "PUT",
           {"percent": 77, "trim": {"right": 99}}, status=400)
    same(res, "and the percent did not change", api.json("/dim").get("percent"),
         now.get("percent"))

    r = expect(res, api, "PUT /dim sweep", "/dim", "PUT", {"sweep": True})
    same(res, "sweeping", r.get("sweeping"), True)
    r = expect(res, api, "a level ends the sweep", "/dim", "PUT", {"percent": 60})
    same(res, "no longer sweeping", r.get("sweeping"), False)

    # Sleep's level is a share of the setting: 60% at a level of 50 is 30%.
    # /sleep last, because it is the one change that does not count as
    # somebody being there -- anything after it would wake the eyes.
    slp: Json = api.json("/sleep")
    if not slp or not slp.get("now"):
        res.skip("sleep dims the setting", "no sleep mode, or no clock")
    else:
        h, m = (int(x) for x in slp["now"].split(":"))
        mins: int = h * 60 + m
        at = lambda o: "%02d:%02d" % (((mins + o) % 1440) // 60, (mins + o) % 60)
        api.json("/sleep", "PUT", {"enabled": True, "start": at(-60),
                                   "stop": at(60), "level": 50})
        same(res, "asleep at level 50 shows 30%", settle_shown(api, 30), 30)
        api.json("/sleep", "PUT", {"enabled": slp["enabled"], "start": slp["start"],
                                   "stop": slp["stop"], "level": slp["level"]})
        same(res, "awake again shows 60%", settle_shown(api, 60), 60)

    restore_dim(api, before)


def restore_dim(api: Api, was: Json) -> None:
    if not was:
        return
    api.raw("/dim", "PUT", {"percent": was.get("percent", 100),
                            "gamma": was.get("gamma", 2.2),
                            "trim": {"left": was.get("trim", {}).get("left", 0),
                                     "right": was.get("trim", {}).get("right", 0)},
                            "sweep": False})


def blank_eye(name: str) -> bytes:
    """A well-formed eye file of all-zero tables: right in every respect
    the board checks, and never meant to be looked at."""
    tables: dict[str, bytes] = {t: bytes(w * n) for t, w, n in make_eye.TABLES}
    return make_eye.build(name, tables)


def slot_unchanged(res: Result, api: Api, label: str, before: Json) -> None:
    """A refused upload must leave the slot exactly as it was."""
    after: Json = api.json("/eyes/slot")
    same(res, label, (after.get("loaded"), after.get("name")),
         (before.get("loaded"), before.get("name")))


def test_eye_slot(res: Result, api: Api, eye_file: str | None) -> None:
    """The eye slot (docs/EYE_FILES.md).  Everything refused here is refused
    before the board erases anything, which is checked, so it runs every
    time.  A real upload writes flash and runs only with --eye-file."""
    res.heading("eye slot")
    slot: Json = expect(res, api, "GET /eyes/slot", "/eyes/slot")
    for key in ("available", "loaded", "capacity"):
        field(res, slot, key, "slot has %s" % key)
    if not slot.get("available"):
        expect(res, api, "upload without a slot is 409", "/eyes/slot", "PUT",
               blank_eye("testeye"), status=409)
        res.skip("the rest of the eye slot",
                 "this board's partition table has no slot")
        return

    good: bytes = blank_eye("testeye")
    expect(res, api, "a JSON body is not an eye file", "/eyes/slot", "PUT",
           {"name": "dragon"}, status=400)
    expect(res, api, "wrong magic is 400", "/eyes/slot", "PUT",
           b"NOTANEYE" + good[8:], status=400)
    newer: bytes = good[:8] + struct.pack("<H", 2) + good[10:]
    expect(res, api, "a newer format is 400", "/eyes/slot", "PUT", newer,
           status=400)
    expect(res, api, "a header alone is 400", "/eyes/slot", "PUT",
           good[:make_eye.HEADER_BYTES], status=400)
    builtin: str = api.json("/eyes").get("designs", [{}])[0].get("name", "")
    expect(res, api, "a built-in's name is 409", "/eyes/slot", "PUT",
           blank_eye(builtin), status=409)
    expect(res, api, "a console keyword is 409", "/eyes/slot", "PUT",
           blank_eye("unload"), status=409)
    slot_unchanged(res, api, "refusals left the slot alone", slot)

    if not eye_file:
        res.skip("uploading, damage and removal",
                 "pass --eye-file dist/eyes/NAME.bin to include them")
        return

    with open(eye_file, "rb") as f:
        data: bytes = f.read()
    name: str = make_eye.check(data)
    damaged: bytearray = bytearray(data)
    damaged[-1] ^= 0xFF
    expect(res, api, "a damaged file is 400", "/eyes/slot", "PUT",
           bytes(damaged), status=400)
    # Past the header, the old design has been erased to make room; damage
    # found at the end leaves the slot empty rather than half-written.
    same(res, "damage leaves it empty",
         api.json("/eyes/slot").get("loaded"), False)

    r: Json = expect(res, api, "PUT /eyes/slot", "/eyes/slot", "PUT", data)
    same(res, "loaded", (r.get("loaded"), r.get("name")), (True, name))
    same(res, "and selected", api.json("/eye").get("name"), name)
    listed: list[Any] = [d for d in api.json("/eyes").get("designs", [])
                         if d.get("loaded")]
    same(res, "listed last, marked loaded",
         [d.get("name") for d in listed], [name])

    r = expect(res, api, "DELETE /eyes/slot", "/eyes/slot", "DELETE")
    same(res, "removed", r.get("loaded"), False)
    same(res, "the eyes moved off it", api.json("/eye").get("index"), 0)

    expect(res, api, "PUT it back", "/eyes/slot", "PUT", data)


def test_gaze(res: Result, api: Api) -> None:
    res.heading("gaze")
    r: Json = expect(res, api, "PUT /gaze", "/gaze", "PUT",
                     {"x": 200, "y": 800})
    same(res, "x round trips", r.get("x"), 200)
    same(res, "y round trips", r.get("y"), 800)
    same(res, "mode became manual", r.get("mode"), "manual")

    for bad, why in (({"x": -1, "y": 0}, "negative"),
                     ({"x": 1024, "y": 0}, "past 1023"),
                     ({"x": 0}, "y missing")):
        expect(res, api, "rejects %s" % why, "/gaze", "PUT", bad, status=400)

    for edge in ({"x": 0, "y": 0}, {"x": 1023, "y": 1023}):
        r = expect(res, api, "accepts the edge %s" % edge, "/gaze", "PUT", edge)

    r = expect(res, api, "PUT /gaze auto", "/gaze", "PUT", {"mode": "auto"})
    same(res, "mode became auto", r.get("mode"), "auto")


def test_gaze_burst(res: Result, api: Api) -> None:
    """What the control page's aim pad does while a finger is down.

    The page coalesces -- newest target only, one request in flight -- because
    a request per pointer event saturates a board that serves one client from
    inside its render loop.  What matters to a client is that the *last*
    position sent is the one that sticks, and that the board stays responsive
    while it happens.
    """
    res.heading("a burst of gaze updates, as a drag produces")
    fps_before: Any = field(res, api.json("/state"), "system.fps", "fps") or 0

    started: float = time.time()
    points: list[tuple[int, int]] = [
        (120 + i * 45, 900 - i * 40) for i in range(16)]
    for (x, y) in points:
        api.raw("/gaze", "PUT", {"x": x, "y": y})
    elapsed: float = time.time() - started

    final: Json = api.json("/gaze")
    lx, ly = points[-1]
    if final.get("x") == lx and final.get("y") == ly:
        res.ok("the last position is the one that sticks", "%d,%d" % (lx, ly))
    else:
        res.fail("the last position is the one that sticks",
                 "board has %s,%s not %d,%d"
                 % (final.get("x"), final.get("y"), lx, ly))

    res.ok("16 updates took", "%.1fs, %.0f ms each" % (elapsed, elapsed / 16 * 1000))

    time.sleep(1.5)
    fps_after: Any = field(res, api.json("/state"), "system.fps", "fps") or 0
    if fps_after >= fps_before * 0.5:
        res.ok("the eyes kept rendering", "%s -> %s fps" % (fps_before, fps_after))
    else:
        res.fail("the eyes kept rendering",
                 "fps fell from %s to %s" % (fps_before, fps_after))
    api.raw("/gaze", "PUT", {"mode": "auto"})


def test_dilate(res: Result, api: Api) -> None:
    res.heading("pupil width")
    r: Json = expect(res, api, "PUT /dilate", "/dilate", "PUT",
                     {"percent": 40})
    same(res, "percent round trips", r.get("percent"), 40)
    same(res, "mode became manual", r.get("mode"), "manual")
    for edge in (0, 100):
        r = expect(res, api, "accepts %d%%" % edge, "/dilate", "PUT",
                   {"percent": edge})
    expect(res, api, "rejects 101", "/dilate", "PUT", {"percent": 101},
           status=400)
    expect(res, api, "rejects -1", "/dilate", "PUT", {"percent": -1},
           status=400)
    r = expect(res, api, "PUT /dilate auto", "/dilate", "PUT", {"mode": "auto"})
    same(res, "mode became auto", r.get("mode"), "auto")


def test_toggles(res: Result, api: Api, start: Json) -> None:
    res.heading("pupil, panel swap and flip")
    for name in ("pupil", "swap"):
        was: Any = field(res, start, "%s.on" % name, name)
        r: Json = expect(res, api, "PUT /%s flips it" % name,
                         "/" + name, "PUT",
                   {"on": not was})
        same(res, "%s is now %s" % (name, not was), r.get("on"), not was)
        r = expect(res, api, "PUT /%s back" % name, "/" + name, "PUT",
                   {"on": was})
        same(res, "%s restored" % name, r.get("on"), was)
        expect(res, api, "/%s rejects a missing body" % name, "/" + name,
               "PUT", {}, status=400)

    # Flip is per side, and the sides are independent.
    flip: Json = start.get("flip", {})
    was_l, was_r = flip.get("left", False), flip.get("right", False)
    r = expect(res, api, "PUT /flip left", "/flip", "PUT", {"left": not was_l})
    same(res, "left flipped", r.get("left"), not was_l)
    same(res, "right untouched", r.get("right"), was_r)
    r = expect(res, api, "PUT /flip both back", "/flip", "PUT",
               {"left": was_l, "right": was_r})
    same(res, "left restored", r.get("left"), was_l)
    same(res, "right restored", r.get("right"), was_r)
    expect(res, api, "/flip rejects a missing body", "/flip", "PUT", {},
           status=400)
    expect(res, api, "/flip rejects a non-bool", "/flip", "PUT",
           {"left": "yes"}, status=400)


def test_clock(res: Result, api: Api, start: Json) -> None:
    res.heading("clock")
    was_on: Any = field(res, start, "clock.on", "clock.on")
    r: Json = expect(res, api, "PUT /clock on", "/clock", "PUT", {"on": True})
    same(res, "clock is on", r.get("on"), True)

    r = expect(res, api, "PUT /clock seconds", "/clock", "PUT",
               {"seconds": True})
    same(res, "second hand on", r.get("seconds"), True)

    r = expect(res, api, "PUT /clock colours", "/clock", "PUT",
               {"colors": {"hour": "112233", "minute": "445566",
                           "second": "778899"}})
    same(res, "hour colour", field(res, r, "colors.hour"), "112233")
    same(res, "minute colour", field(res, r, "colors.minute"), "445566")
    same(res, "second colour", field(res, r, "colors.second"), "778899")

    r = expect(res, api, "PUT /clock rate", "/clock", "PUT", {"rate": 60})
    same(res, "rate round trips", r.get("rate"), 60)
    expect(res, api, "rejects rate 0", "/clock", "PUT", {"rate": 0}, status=400)
    expect(res, api, "rejects rate 3601", "/clock", "PUT", {"rate": 3601},
           status=400)
    expect(res, api, "rejects a bad colour", "/clock", "PUT",
           {"colors": {"hour": "gggggg"}}, status=400)
    expect(res, api, "rejects a bad time", "/clock", "PUT",
           {"time": "not-a-time"}, status=400)

    # Only meaningful while nothing outranks a hand-set time.
    src: Any = field(res, api.json("/state"), "time.source", "time.source")
    if src == "ntp":
        res.skip("PUT /clock time", "a time server is in charge and outranks it")
    else:
        r = expect(res, api, "PUT /clock time", "/clock", "PUT",
                   {"time": "04:05:06"})
        if str(r.get("time", "")).startswith("04:05:0"):
            res.ok("the time took", r.get("time"))
        else:
            res.fail("the time took", "reads %s" % r.get("time"))

    # Put the display settings back the way they were.
    api.raw("/clock", "PUT", {
        "on": was_on,
        "seconds": field(res, start, "clock.seconds", "clock.seconds"),
        "rate": field(res, start, "clock.rate", "clock.rate"),
        "colors": start.get("clock", {}).get("colors", {}),
    })


def test_time(res: Result, api: Api) -> None:
    res.heading("timezone and time sources")
    tz: Json = expect(res, api, "GET /tz", "/tz")
    zones: list[Any] = tz.get("zones", [])
    if len(zones) < 10:
        res.fail("zone list", "only %d zones" % len(zones))
    else:
        regions: list[str] = sorted({z["region"] for z in zones})
        res.ok("zones offered", "%d across %d regions" % (len(zones), len(regions)))
    # Name and region only: the POSIX string was two thirds of this reply and
    # nothing reads it -- a client picks a name and sends the name back.
    for z in zones[:3]:
        for key in ("name", "region"):
            if key not in z:
                res.fail("zone entries have %s" % key, repr(z)[:50])
        if "tz" in z:
            res.fail("zone entries are lean", "still carrying the POSIX string")

    was: Any = tz.get("tz")
    r: Json = expect(res, api, "PUT /tz by city", "/tz", "PUT",
                     {"tz": "tokyo"})
    same(res, "tokyo applied", r.get("tz"), "JST-9")

    r = expect(res, api, "PUT /tz by old regional name", "/tz", "PUT",
               {"tz": "pacific"})
    if str(r.get("tz", "")).startswith("PST8PDT"):
        res.ok("the older names still work", r.get("tz"))
    else:
        res.fail("the older names still work", "got %r" % r.get("tz"))

    r = expect(res, api, "PUT /tz raw POSIX", "/tz", "PUT",
               {"tz": "<+0545>-5:45"})
    same(res, "an unlisted zone still works", r.get("tz"), "<+0545>-5:45")

    expect(res, api, "rejects an over-long zone", "/tz", "PUT",
           {"tz": "X" * 80}, status=400)
    api.raw("/tz", "PUT", {"tz": was})
    res.ok("timezone restored", was)


def test_ntp(res: Result, api: Api) -> None:
    res.heading("the time client")
    n: Json = expect(res, api, "GET /ntp", "/ntp")
    ntp: Any = n.get("ntp", {})
    for key in ("available", "enabled", "running", "linkUp", "synced",
                "intervalSeconds", "server"):
        if key not in ntp:
            res.fail("/ntp reports %s" % key, "absent")
    if ntp:
        res.ok("time client", "%s, %s, every %sh via %s"
               % ("on" if ntp.get("enabled") else "off",
                  "synced" if ntp.get("synced") else "not synced",
                  int(ntp.get("intervalSeconds", 0)) // 3600,
                  ntp.get("server")))
    expect(res, api, "rejects a bad op", "/ntp", "PUT", {"op": "nope"},
           status=400)

    if ntp.get("enabled") and ntp.get("linkUp"):
        before: Any = ntp.get("lastSyncSeconds")
        # The timezone tests just before restart the client, which asks a
        # server at once, so a sync has often only just landed.  Asking again
        # straight away proves nothing: from 0 the counter has nowhere to fall,
        # and a server may ignore a second request that close.  So let the
        # last one age first.
        settle: float = time.time() + 15
        while before is not None and before < 8 and time.time() < settle:
            time.sleep(1)
            before = api.json("/ntp").get("ntp", {}).get("lastSyncSeconds")
        expect(res, api, "PUT /ntp sync now", "/ntp", "PUT", {"op": "sync"})
        # A pool server has to be resolved and then asked over the internet,
        # which is not bounded by any particular number of seconds -- so wait
        # for the counter to reset rather than guessing how long it takes.
        after: Any = before
        deadline: float = time.time() + 20
        while time.time() < deadline:
            time.sleep(2)
            after = api.json("/ntp").get("ntp", {}).get("lastSyncSeconds")
            if after is not None and (before is None or after < before):
                break
        if after is not None and (before is None or after < before):
            res.ok("a server answered", "last sync %ss ago, was %ss" % (after, before))
        else:
            res.fail("a server answered",
                     "counter did not reset within 20s (%ss then %ss)"
                     % (before, after))
    else:
        res.skip("PUT /ntp sync now", "the client is off or there is no link")


def test_rtc(res: Result, api: Api, info: Json) -> None:
    res.heading("battery-backed clock")
    if not info.get("rtc"):
        res.skip("GET /rtc", "not compiled into this firmware")
        expect(res, api, "/rtc is absent without RTC=1", "/rtc", status=404)
        return
    r: Json = expect(res, api, "GET /rtc", "/rtc")
    for key in ("present", "valid"):
        if key not in r:
            res.fail("/rtc reports %s" % key, "absent")
    if not r.get("present"):
        res.ok("no module on the bus", "reported honestly, not as an error")
        expect(res, api, "sync without a module is 404", "/rtc", "PUT",
               {"op": "sync"}, status=404)
        return
    res.ok("module present", "valid=%s" % r.get("valid"))
    if "temperatureC" in r:
        res.ok("chip temperature", "%.2f C" % r["temperatureC"])
    expect(res, api, "rejects a bad op", "/rtc", "PUT", {"op": "nope"},
           status=400)


def test_netinfo(res: Result, api: Api) -> None:
    res.heading("address cards on the panels")
    r: Json = expect(res, api, "PUT /netinfo on", "/netinfo", "PUT",
                     {"on": True})
    same(res, "cards are up", r.get("on"), True)
    if field(res, api.json("/state"), "net.showingInfo", "showingInfo") is True:
        res.ok("state agrees the cards are up")
    r = expect(res, api, "PUT /netinfo off", "/netinfo", "PUT", {"on": False})
    same(res, "cards dismissed", r.get("on"), False)
    expect(res, api, "rejects a missing body", "/netinfo", "PUT", {},
           status=400)


def test_actions(res: Result, api: Api) -> None:
    res.heading("momentary actions")
    for action in ("blink", "startle", "splash", "netinfo"):
        expect(res, api, "POST /action %s" % action, "/action", "POST",
               {"action": action})
        time.sleep(0.3)
    expect(res, api, "rejects an unknown action", "/action", "POST",
           {"action": "explode"}, status=400)
    expect(res, api, "rejects an empty body", "/action", "POST", {},
           status=400)
    api.raw("/netinfo", "PUT", {"on": False})


def test_errors(res: Result, api: Api) -> None:
    res.heading("saying no properly")
    expect(res, api, "unknown path is 404", "/nosuchthing", status=404)
    expect(res, api, "wrong verb on a real path is 405", "/eye", "POST",
           {"next": True}, status=405)
    expect(res, api, "wrong verb on a read-only path is 405", "/state", "PUT",
           {}, status=405)

    # A body that is not JSON, and one that is not sent as JSON at all.
    code, raw = api.raw("/gaze", "PUT", None,
                        {"Content-Type": "application/json"})
    if code == 400:
        res.ok("a missing body is 400", "400")
    else:
        res.fail("a missing body is 400", "got %s" % code)

    req: urllib.request.Request = urllib.request.Request(
        api.base + "/gaze", method="PUT")
    req.add_header("Content-Type", "application/json")
    if api.token:
        req.add_header("Authorization", "Bearer " + api.token)
    try:
        with api.opener.open(req, b"{not json", timeout=api.timeout) as r:
            code: int = r.status
    except urllib.error.HTTPError as e:
        code = e.code
    except Exception:  # noqa: BLE001
        code = 0
    if code == 400:
        res.ok("malformed JSON is 400", "400")
    else:
        res.fail("malformed JSON is 400", "got %s" % code)

    err: Json = api.json("/eye", "PUT", {"name": "nosucheye"})
    if isinstance(err.get("error"), str) and err["error"]:
        res.ok("errors carry a reason", err["error"][:44])
    else:
        res.fail("errors carry a reason", repr(err)[:50])


def raw_reply(host: str, data: bytes, wait: float = 10.0) -> tuple[int | None, float]:
    """Send bytes on a fresh connection; return the reply's status (None if
    the board closed without one) and how long that took.  For requests
    urllib will not send: a malformed one, or one that never finishes."""
    name, _, port = host.partition(":")
    started: float = time.time()
    reply: bytes = b""
    try:
        with socket.create_connection((name, int(port or 80)), timeout=wait) as s:
            s.sendall(data)
            while b"\r\n" not in reply:
                chunk: bytes = s.recv(4096)
                if not chunk:
                    break
                reply += chunk
    except OSError:
        pass
    m: re.Match[bytes] | None = re.match(rb"HTTP/1\.[01] (\d{3})", reply)
    return (int(m.group(1)) if m else None, time.time() - started)


def test_request_gate(res: Result, api: Api, host: str) -> None:
    """A request is only taken once all of it has arrived, because taking it
    blocks the render loop; one that cannot fit is refused unread.  None of
    this reaches a credential check, so it needs none."""
    res.heading("taking a request")
    head: bytes = ("Host: %s\r\n" % host).encode()
    code, _ = raw_reply(host, b"GET /api/v1/state HTTP/1.1\r\n" + head +
                        b"X-Pad: " + b"a" * 4000 + b"\r\n\r\n")
    same(res, "a request head too large is 431", code, 431)
    code, _ = raw_reply(host, b"PUT /api/v1/dim HTTP/1.1\r\n" + head +
                        b"Content-Type: application/json\r\n"
                        b"Content-Length: 100000\r\n\r\n")
    same(res, "a body too large is 413, before any of it is sent", code, 413)
    code, _ = raw_reply(host, b"PUT /api/v1/dim HTTP/1.1\r\n" + head +
                        b"Content-Length: lots\r\n\r\n")
    same(res, "an unreadable Content-Length is 400", code, 400)

    # A request that stops arriving partway is dropped, and the eyes keep
    # moving while it waits -- before, the board waited on it from inside
    # the render loop.
    # The baseline has to be a rendering one: an earlier test may have left
    # cards on the panels, which draw no frames.
    before: int = 0
    for _ in range(8):
        before = api.json("/state").get("system", {}).get("fps") or 0
        if before:
            break
        time.sleep(1)
    code, took = raw_reply(host, b"GET /api/v1/state HTTP/1.1\r\n" + head)
    if code is None and 3.0 <= took <= 8.0:
        res.ok("a request that never finishes is dropped", "after %.1fs" % took)
    else:
        res.fail("a request that never finishes is dropped",
                 "status %r after %.1fs" % (code, took))
    after: Any = api.json("/state").get("system", {}).get("fps")
    if not before:
        res.skip("the eyes kept moving while it waited",
                 "the eyes are not drawing (sleep, off, or cards)")
    elif isinstance(after, int) and after >= before * 0.8:
        res.ok("the eyes kept moving while it waited", "%s -> %s fps" % (before, after))
    else:
        res.fail("the eyes kept moving while it waited", "%r -> %r fps" % (before, after))


def test_cors(res: Result, api: Api, info: Json) -> None:
    res.heading("cross-origin")
    code, _ = api.raw("/eye", "OPTIONS")
    if code == 204:
        res.ok("preflight is answered", "204")
    else:
        res.fail("preflight is answered", "got %s" % code)

    req: urllib.request.Request = urllib.request.Request(
        api.base + "/eye", method="OPTIONS")
    try:
        with api.opener.open(req, timeout=api.timeout) as r:
            origin: str | None = r.headers.get(
                "Access-Control-Allow-Origin")
            methods: str | None = r.headers.get(
                "Access-Control-Allow-Methods")
    except Exception as e:  # noqa: BLE001
        res.fail("preflight headers", repr(e)[:50])
        return
    if info.get("auth"):
        if origin is None:
            res.ok("no wildcard origin while a credential is required")
        else:
            res.fail("no wildcard origin while a credential is required",
                     "sent %r" % origin)
    else:
        same(res, "wildcard origin on an open board", origin, "*")
    if methods and "PUT" in methods:
        res.ok("allowed methods advertised", methods)
    else:
        res.fail("allowed methods advertised", repr(methods))

    # The page refuses to be framed by anyone, on every build.
    req = urllib.request.Request(api.root + "/")
    if api.token:
        req.add_header("Authorization", "Bearer " + api.token)
    try:
        with api.opener.open(req, timeout=api.timeout) as r:
            xfo: str | None = r.headers.get("X-Frame-Options")
            csp: str | None = r.headers.get("Content-Security-Policy")
    except Exception as e:  # noqa: BLE001
        res.fail("the page's framing headers", repr(e)[:50])
        return
    same(res, "the page refuses framing (X-Frame-Options)", xfo, "DENY")
    same(res, "the page refuses framing (CSP)", csp, "frame-ancestors 'none'")


def test_auth(res: Result, api: Api, info: Json, host: str) -> None:
    res.heading("authentication")
    if not info.get("auth"):
        res.skip("unauthenticated requests", "this build requires no credential")
        return
    bare: urllib.request.OpenerDirector = urllib.request.build_opener()
    for path in ("/api/v1/state", "/"):
        req: urllib.request.Request = urllib.request.Request(
            "http://%s%s" % (host, path))
        try:
            with bare.open(req, timeout=api.timeout) as r:
                code: int = r.status
        except urllib.error.HTTPError as e:
            code = e.code
        except Exception as e:  # noqa: BLE001
            res.fail("%s without a credential" % path, repr(e)[:50])
            continue
        if code == 401:
            res.ok("%s without a credential is 401" % path, "401")
        else:
            res.fail("%s without a credential is 401" % path, "got %s" % code)

    # A request the browser says another site sent is refused before any
    # credential is considered; the same credential works without it.  Every
    # request here is a GET of /state or of the page, so nothing changes.
    for label, hdrs in (
            ("Sec-Fetch-Site cross-site", {"Sec-Fetch-Site": "cross-site"}),
            ("Sec-Fetch-Site same-site", {"Sec-Fetch-Site": "same-site"}),
            ("an Origin of another site", {"Origin": "http://example.com"}),
            ("a Referer from another site",
             {"Referer": "http://example.com/page"})):
        code, _ = api.raw("/state", headers=hdrs)
        same(res, "refused with %s" % label, code, 403)
    for label, hdrs in (
            ("Sec-Fetch-Site same-origin", {"Sec-Fetch-Site": "same-origin"}),
            ("Sec-Fetch-Site none", {"Sec-Fetch-Site": "none"}),
            ("this device's own Origin", {"Origin": api.root})):
        code, _ = api.raw("/state", headers=hdrs)
        same(res, "allowed with %s" % label, code, 200)
    link: dict[str, str] = {"Sec-Fetch-Site": "cross-site",
                            "Sec-Fetch-Dest": "document"}
    code, _ = api.raw("/", headers=link, full=True)
    same(res, "a link from another site still opens the page", code, 200)
    code, _ = api.raw("/", headers=dict(link, **{"Sec-Fetch-Dest": "iframe"}),
                      full=True)
    same(res, "another site cannot frame the page", code, 403)

    # A digest answer opens only the request it was computed for.  The
    # library checked it against the uri the header names, whatever the
    # request actually was, so one header seen on the network opened every
    # route with the same method.
    if not (api.user and api.password):
        res.skip("a digest answer is bound to its request",
                 "needs --user and --password")
        return
    probe: str = "/api/v1/state?probe=1"
    header: str | None = digest_header(host, probe, api.user, api.password)
    if header is None:
        res.fail("a digest answer is bound to its request", "no challenge")
        return

    def status_with(path: str) -> int | None:
        req: urllib.request.Request = urllib.request.Request(
            "http://%s%s" % (host, path))
        req.add_header("Authorization", header)
        try:
            with urllib.request.build_opener().open(
                    req, timeout=api.timeout) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code
        except Exception:  # noqa: BLE001
            return None

    same(res, "a digest answer works for its own request",
         status_with(probe), 200)
    same(res, "... and is refused on another path",
         status_with("/api/v1/info"), 401)
    same(res, "... and with another query",
         status_with("/api/v1/state?probe=2"), 401)


def test_credentials(res: Result, api: Api, info: Json,
                     args: argparse.Namespace) -> None:
    """The credential endpoints.

    Everything here except the last group is read-only or expected to be
    refused, which is deliberate: a test that changes a password and then dies
    before changing it back leaves a board nobody can talk to.  The part that
    actually changes one runs only under --credentials, sets a password it
    knows, and puts the original back.
    """
    res.heading("credentials")

    r: int = api.raw("/credentials")[0]
    if r == 404:
        res.skip("the credential endpoints", "not compiled into this firmware")
        return

    c: Json = api.json("/credentials")
    if not c:
        res.fail("GET /credentials", "no body")
        return

    # The point of the endpoint is to report without revealing.  Anything that
    # looks like a secret coming back is a bug worth failing loudly for.
    blob: str = json.dumps(c).lower()
    leaked: list[str] = [
        w for w in ("password", "secret", "hash") if '"%s"' % w in blob]
    # "password" may legitimately appear as a *key* name; what must not appear
    # is any credential we know the value of.
    known: list[str] = [v for v in (args.password, args.token) if v]
    spilled: list[str] = [v for v in known if v and v.lower() in blob]
    if spilled:
        res.fail("GET /credentials leaks a credential",
                 "the response contains a value we authenticated with")
    else:
        res.ok("reveals no credential", "reports state, not secrets")
    if leaked:
        res.ok("mentions %s" % ", ".join(leaked), "as field names only")

    for k in ("http", "token", "ota"):
        if isinstance(c.get(k), dict) and "required" in c[k]:
            res.ok("reports %s" % k,
                   "required=%s" % c[k]["required"])
        else:
            res.fail("GET /credentials", "no %s.required in the body" % k)

    if c.get("changeable") is False:
        res.skip("changing a credential",
                 "this build's API requires none, so it refuses to set any")
        expect(res, api, "refuses to change without auth", "/credentials",
               "PUT", {"password": "whatever"}, status=403)
        return

    # Refusals.  None of these change anything.
    # Without the management password there is nothing to put in "current",
    # and the board checks identity before it validates input -- correctly, so
    # these would all come back 403 and prove nothing.
    if c.get("http", {}).get("required") and not args.password:
        res.skip("the refusal cases", "pass --password to include them")
        return

    # An empty body means something different depending on the build.  With
    # AUTH_HTTP the "current" check comes first -- deliberately, so that a page
    # you merely visited cannot change a password using credentials the browser
    # attaches on its own -- and an absent "current" fails it, 403.  Without
    # AUTH_HTTP there is no such check to fail, so the body falls through to
    # "nothing to change", 400.  Both are right; only one is true at a time.
    expect(res, api, "rejects an empty change", "/credentials", "PUT", {},
           status=403 if c.get("http", {}).get("required") else 400)
    if c.get("http", {}).get("required"):
        expect(res, api, "rejects a wrong current password", "/credentials",
               "PUT", {"current": "definitely-not-it", "password": "xyzzy123"},
               status=403)
        expect(res, api, "rejects an empty new password", "/credentials",
               "PUT", {"current": args.password or "", "password": ""},
               status=400)
        expect(res, api, "rejects a colon in the username", "/credentials",
               "PUT", {"current": args.password or "", "user": "a:b"},
               status=400)

    if not args.credentials:
        res.skip("actually changing a password",
                 "pass --credentials to include it")
        return

    # The real thing, and then back again.
    if not (c.get("http", {}).get("required") and args.password):
        res.skip("the round trip", "needs an AUTH_HTTP build and --password")
        return

    temp: str = "frank-test-" + str(int(time.time()))
    r = api.json("/credentials", "PUT",
                 {"current": args.password, "password": temp})
    if not r or not r.get("ok"):
        res.fail("PUT /credentials", "the change was refused")
        return
    res.ok("changed the page password")

    # The old credential must stop working, or nothing was really changed.
    api.recredential(args.user, temp)
    after: Json = api.json("/credentials")
    if after:
        res.ok("the new password works")
    else:
        res.fail("the new password", "the board did not accept it")

    back: Json = api.json("/credentials", "PUT",
                    {"current": temp, "password": args.password})
    api.recredential(args.user, args.password)
    if back and back.get("ok"):
        res.ok("put the original password back")
    else:
        res.fail("restoring the password",
                 "THE BOARD IS NOW ON %r -- write that down" % temp)


def test_sleep(res: Result, api: Api) -> None:
    """The overnight window.

    The interesting cases are the ones a naive implementation gets wrong: a
    window that crosses midnight, and a board that does not know the time.
    Both are checked here without waiting for either to happen, by setting a
    window around the board's own clock and reading back what it concludes.
    """
    res.heading("sleep")

    code: int = api.raw("/sleep")[0]
    if code == 404:
        res.skip("the sleep endpoints", "not compiled into this firmware")
        return

    before: Json = api.json("/sleep")
    if not before:
        res.fail("GET /sleep", "no body")
        return
    for k in ("enabled", "start", "stop", "level", "asleep", "reason"):
        if k not in before:
            res.fail("GET /sleep", "no %s in the body" % k)
            return
    res.ok("reports the window", "%s-%s, %s"
           % (before["start"], before["stop"], before["reason"]))

    expect(res, api, "rejects a bad start", "/sleep", "PUT",
           {"start": "25:00"}, status=400)
    expect(res, api, "rejects a bad stop", "/sleep", "PUT",
           {"stop": "nonsense"}, status=400)
    expect(res, api, "rejects a bad level", "/sleep", "PUT",
           {"level": 101}, status=400)
    expect(res, api, "rejects an empty change", "/sleep", "PUT", {},
           status=400)

    # What time does the board think it is?  Everything below is relative to
    # that, so the test works whatever the zone and whatever the hour.
    #
    # Taken from /sleep's own "now" rather than reconstructed from the clock
    # card: clock.secondOfDay can be running at an accelerated rate for
    # testing, and would quietly put every window in the wrong place.
    hhmm: Any = before.get("now")
    known: bool = bool(hhmm)

    if not known:
        # The guard is the feature: no time, no sleeping.
        r: Json = api.json("/sleep", "PUT", {"enabled": True})
        if r.get("reason") == "waiting for the time":
            res.ok("will not sleep without a clock", r.get("reason"))
        else:
            res.fail("the no-clock guard",
                     "enabled with no time source and said %r" % r.get("reason"))
        api.json("/sleep", "PUT", {"enabled": before["enabled"]})
        return

    def minutes(text: str) -> int:
        h, m = text.split(":")[:2]
        return int(h) * 60 + int(m)

    now: int = minutes(str(hhmm))

    def at(offset: int) -> str:
        v = (now + offset + 1440) % 1440
        return "%02d:%02d" % (v // 60, v % 60)

    # A window that started an hour ago and ends in an hour: the board should
    # say it is asleep.  Level 100 so the panels stay lit while we do it --
    # turning them off during a test is unhelpful to anyone watching.
    r = api.json("/sleep", "PUT",
                 {"enabled": True, "start": at(-60), "stop": at(60),
                  "level": 100})
    same(res, "inside the window it sleeps", r.get("asleep"), True)

    # The countdown, and that /state carries the same one.  The page renders
    # its card from /state once a second, and a countdown only /sleep knew
    # once sat frozen under a live reason: "asleep -- sleeps in 2h 4m".
    # 59 is allowed: the clock may have ticked over since "now" was read.
    if (r.get("changesInMinutes") in (59, 60)
            and r.get("changesToAsleep") is False):
        res.ok("counts down to the end of the window",
               "wakes in %dm" % r["changesInMinutes"])
    else:
        res.fail("the countdown", "changesInMinutes=%r changesToAsleep=%r"
                 % (r.get("changesInMinutes"), r.get("changesToAsleep")))
    # Read second, so the minute may have ticked over in between: one less
    # is the same countdown.
    st: Json = api.json("/state").get("sleep", {})
    if (r.get("changesInMinutes") is not None
            and st.get("changesInMinutes") in (r["changesInMinutes"],
                                               r["changesInMinutes"] - 1)
            and st.get("changesToAsleep") == r.get("changesToAsleep")):
        res.ok("/state carries the same countdown")
    else:
        res.fail("/state carries the same countdown",
                 "state says %r/%r, sleep says %r/%r"
                 % (st.get("changesInMinutes"), st.get("changesToAsleep"),
                    r.get("changesInMinutes"), r.get("changesToAsleep")))

    # Now one that ended an hour ago.
    r = api.json("/sleep", "PUT", {"start": at(-120), "stop": at(-60)})
    same(res, "outside the window it wakes", r.get("asleep"), False)

    # The case that matters: a window running across midnight, positioned so
    # that "now" is inside it.  If the comparison is the naive start<stop one,
    # this reads as awake and the test catches it.
    across: Json = api.json(
        "/sleep", "PUT", {"start": at(-60), "stop": at(-120)})
    if across.get("asleep") is True:
        res.ok("a window across midnight", "%s-%s and asleep"
               % (across.get("start"), across.get("stop")))
    else:
        res.fail("a window across midnight",
                 "%s-%s reported awake" % (across.get("start"),
                                           across.get("stop")))

    # Equal times mean never, not always.
    r = api.json("/sleep", "PUT", {"start": at(0), "stop": at(0)})
    same(res, "a zero-length window never sleeps", r.get("asleep"), False)

    # A state-changing request should have woken it; a poll should not.  Put
    # it back inside the window first.
    api.json("/sleep", "PUT", {"start": at(-60), "stop": at(60)})
    api.json("/gaze", "PUT", {"x": 512, "y": 512})
    r = api.json("/sleep")
    if r.get("reason") == "woken":
        res.ok("a command wakes it", "for the hold-off period")
    elif r.get("reason") == "asleep":
        res.skip("waking on a command", "SLEEP_WAKE_S is 0 in this build")
    else:
        res.fail("waking on a command", "reason was %r" % r.get("reason"))

    api.json("/sleep", "PUT",
             {"enabled": before["enabled"], "start": before["start"],
              "stop": before["stop"], "level": before["level"]})
    res.ok("put the window back", "%s-%s" % (before["start"], before["stop"]))


def test_page(res: Result, api: Api) -> None:
    res.heading("the control page")
    code, body = api.raw("/", full=True)
    if code != 200:
        res.fail("GET /", "got %s" % code)
        return

    # Served gzipped, unconditionally -- every browser has understood it for
    # decades, and carrying a second uncompressed copy in flash to satisfy a
    # client that does not exist would be the wrong trade.
    if body[:2] == b"\x1f\x8b":
        try:
            plain: bytes = gzip.decompress(body)
        except Exception as e:
            res.fail("the gzip stream decompresses", repr(e)[:50])
            return
        res.ok("GET /", "%d bytes gzipped, %d unpacked (%.0f%%)"
               % (len(body), len(plain), 100.0 * len(body) / len(plain)))
        text: str = plain.decode("utf-8", "replace")
    else:
        res.fail("the page is compressed", "%d bytes, not gzip" % len(body))
        text = body.decode("utf-8", "replace")
    for what, needle in (("a title", "<title>frank</title>"),
                         ("a tab icon", "rel=icon"),
                         ("the state poll", "/api/v1"),
                         ("the aim pad", "id=pad")):
        if needle in text:
            res.ok("page has %s" % what)
        else:
            res.fail("page has %s" % what, "%r not found" % needle)


def test_settings(res: Result, api: Api) -> None:
    res.heading("saving (writes flash)")
    expect(res, api, "POST /settings save", "/settings", "POST",
           {"op": "save"})
    dirty: Any = field(res, api.json("/state"), "system.settingsDirty",
                       "dirty")
    same(res, "nothing unsaved afterwards", dirty, False)
    expect(res, api, "rejects a bad op", "/settings", "POST", {"op": "nope"},
           status=400)


def test_wifi(res: Result, api: Api) -> None:
    res.heading("wifi (reboots the board)")
    r: Json = expect(res, api, "GET /wifi", "/wifi")
    for key in ("state", "ssid", "stored", "portalName"):
        if key not in r:
            res.fail("/wifi reports %s" % key, "absent")
    blob: str = json.dumps(r).lower()
    if "pass" in blob and "password" not in blob:
        res.fail("no password in the reply", blob[:60])
    else:
        res.ok("no password comes back", "ssid=%r stored=%r"
               % (r.get("ssid"), r.get("stored")))
    expect(res, api, "rejects an empty ssid", "/wifi", "PUT", {"ssid": ""},
           status=400)
    expect(res, api, "rejects an unknown op", "/wifi", "PUT", {"op": "nope"},
           status=400)


def test_cpu(res: Result, api: Api) -> None:
    """The CPU speed, read-only: a change is stored at once and takes effect at
    the next restart, so changing it is left to --restart."""
    res.heading("CPU speed")
    code, _ = api.raw("/cpu")
    if code == 404:
        res.skip("GET /cpu", "this firmware has no CPU speed setting")
        return
    r: Json = expect(res, api, "GET /cpu", "/cpu")
    for key in ("mhz", "setting"):
        if r.get(key) not in (160, 240):
            res.fail("%s is 160 or 240" % key, "got %r" % r.get(key))
    st: Json = api.json("/state").get("system", {})
    same(res, "/state carries the speed", st.get("cpuMhz"), r.get("mhz"))
    same(res, "/state carries the setting", st.get("cpuSetting"),
         r.get("setting"))
    # Refused before anything is stored, so none of these writes flash.
    for label, body in (("200 MHz", {"mhz": 200}),
                        ("80 MHz", {"mhz": 80}),
                        ("a string", {"mhz": "fast"}),
                        ("a fraction", {"mhz": 160.5}),
                        ("a missing body", {})):
        expect(res, api, "/cpu rejects %s" % label, "/cpu", "PUT", body,
               status=400)


def wait_for_restart(api: Api, up_before: float, within: float = 90.0) -> Any:
    """The board's uptime once it answers again with less than it had, or
    None if it does not within the time."""
    deadline: float = time.time() + within
    time.sleep(4)  # it answers for up to RESTART_DELAY_MS first
    while time.time() < deadline:
        up: Any = api.json("/state").get("system", {}).get("uptimeSeconds")
        if up is not None and up < up_before:
            return up
        time.sleep(2)
    return None


def restart(res: Result, api: Api, label: str) -> bool:
    before: Any = api.json("/state").get("system", {}).get("uptimeSeconds")
    expect(res, api, "POST /action restart (%s)" % label, "/action", "POST",
           {"action": "restart"})
    up: Any = wait_for_restart(api, before if before is not None else 10 ** 9)
    if up is None:
        res.fail("back after the restart (%s)" % label, "not within 90 s")
        return False
    res.ok("back after the restart (%s)" % label, "up %ss" % up)
    return True


def test_restart(res: Result, api: Api) -> None:
    """A restart, and the CPU speed across one.  Reboots twice and writes the
    setting to flash, and puts it back."""
    res.heading("restart and CPU speed (reboots, writes flash)")
    r: Json = api.json("/cpu")
    was: Any = r.get("setting")
    if was not in (160, 240):
        res.skip("the CPU speed across a restart", "no CPU speed setting")
        return
    other: int = 240 if was == 160 else 160
    r = expect(res, api, "PUT /cpu %d" % other, "/cpu", "PUT", {"mhz": other})
    same(res, "stored", r.get("setting"), other)
    same(res, "not applied until the restart", r.get("mhz"), was)
    if not restart(res, api, "to %d" % other):
        return
    r = api.json("/cpu")
    same(res, "running at %d" % other, r.get("mhz"), other)
    st: Json = api.json("/state").get("system", {})
    res.ok("drawing", "%s fps at %s MHz" % (st.get("fps"), r.get("mhz")))
    same(res, "a planned restart leaves no warning",
         [w for w in st.get("warnings", []) if "restarted" in w], [])
    expect(res, api, "PUT /cpu back", "/cpu", "PUT", {"mhz": was})
    if restart(res, api, "back to %d" % was):
        same(res, "running at %d again" % was, api.json("/cpu").get("mhz"), was)


def restore(api: Api, start: Json) -> None:
    """Put back what the run changed."""
    api.raw("/eye", "PUT", {"index": start.get("eye", {}).get("index", 0)})
    api.raw("/gaze", "PUT", {"mode": start.get("gaze", {}).get("mode", "auto")}
            if start.get("gaze", {}).get("mode") == "auto" else
            {"x": start["gaze"]["x"], "y": start["gaze"]["y"]})
    api.raw("/dilate", "PUT",
            {"mode": "auto"} if start.get("dilate", {}).get("mode") == "auto"
            else {"percent": start["dilate"]["percent"]})
    api.raw("/pupil", "PUT", {"on": start.get("pupil", {}).get("on", True)})
    api.raw("/swap", "PUT", {"on": start.get("swap", {}).get("on", False)})
    flip: Any = start.get("flip", {})
    api.raw("/flip", "PUT", {"left": flip.get("left", False),
                             "right": flip.get("right", False)})
    restore_dim(api, start.get("dim", {}))
    clock: Any = start.get("clock", {})
    if clock:
        api.raw("/clock", "PUT", {"on": clock.get("on", False),
                                  "seconds": clock.get("seconds", False),
                                  "rate": clock.get("rate", 1),
                                  "colors": clock.get("colors", {})})
    api.raw("/netinfo", "PUT", {"on": False})


def measure(res: Result, api: Api, n: int) -> int:
    """Time each endpoint repeatedly and report, without testing anything.

    For answering "did that change help", which a functional run cannot: it
    visits each endpoint once or twice, and one sample of a noisy number is
    not a measurement.
    """
    probes: list[tuple[str, str, Json | None]] = [
        ("/eye", "GET", None),
        ("/eyes", "GET", None),
        ("/state", "GET", None),
        ("/net", "GET", None),
        ("/info", "GET", None),
        ("/tz", "GET", None),
        ("/ntp", "GET", None),
        ("/gaze", "PUT", {"x": 500, "y": 500}),
        ("/eye", "PUT", {"index": 0}),
        ("/action", "POST", {"action": "blink"}),
        ("/eye", "OPTIONS", None),
    ]
    print("timing %d requests per endpoint against %s\n" % (n, api.base))
    for path, method, body in probes:
        for _ in range(n):
            api.raw(path, method, body)
            time.sleep(0.15)
    # The page is the largest single transfer and worth its own line.
    for _ in range(max(3, n // 3)):
        api.raw("/", full=True)
        time.sleep(0.2)
    api.raw("/gaze", "PUT", {"mode": "auto"})
    report_latency(res)
    return 0


def percentile(values: list[float], p: float) -> float:
    """Nearest-rank, which needs no interpolation and no numpy."""
    if not values:
        return 0.0
    k: int = max(1, min(len(values), int(round(p / 100.0 * len(values)))))
    return sorted(values)[k - 1]


def histogram(values: list[float], width: int = 42) -> None:
    """Log-ish buckets, because the interesting spread is at the tail.

    A linear histogram of this data is one tall bar and a lot of empty space;
    the whole question is how far the slow end reaches.
    """
    edges: list[int] = [0, 20, 30, 40, 50, 65, 80, 100, 150, 250, 500,
                        1000, 1 << 30]
    labels: list[str] = ["   <20", " 20-30", " 30-40", " 40-50", " 50-65",
                         " 65-80",
              " 80-100", "100-150", "150-250", "250-500", "0.5-1s", "  >1s"]
    counts: list[int] = [0] * (len(edges) - 1)
    for v in values:
        for i in range(len(counts)):
            if edges[i] <= v < edges[i + 1]:
                counts[i] += 1
                break
    top: int = max(counts) or 1
    for label, n in zip(labels, counts):
        if not n:
            continue
        bar: str = "#" * max(1, int(round(n * width / top)))
        print("    %7s ms  %-*s %4d" % (label, width, bar, n))


def report_latency(res: Result) -> None:
    """What the run looked like from the client's side.

    Reported per endpoint as well as overall, because on this board the size
    of a reply matters more than what the handler did to produce it, and a
    single aggregate hides which endpoint is the expensive one.
    """
    if not res.samples:
        return
    allms: list[float] = [ms for ms, _ in res.samples]
    print()
    print("%d requests: median %.0f ms, p90 %.0f, p99 %.0f, max %.0f (%s)"
          % (len(allms), percentile(allms, 50), percentile(allms, 90),
             percentile(allms, 99), res.slowest, res.slowest_what))

    print()
    print("  distribution")
    histogram(allms)

    by: dict[str, list[float]] = {}
    for ms, what in res.samples:
        by.setdefault(what, []).append(ms)
    rows: list[tuple[str, list[float]]] = sorted(
        by.items(), key=lambda kv: percentile(kv[1], 50),
                  reverse=True)
    print()
    print("  slowest endpoints          n   median      p90      max")
    for what, vals in rows[:10]:
        print("  %-24s %3d %8.0f %8.0f %8.0f"
              % (what, len(vals), percentile(vals, 50), percentile(vals, 90),
                 max(vals)))

    if res.slow:
        print()
        print("  over a second -- each one is a stalled render loop:")
        for ms, what in sorted(res.slow, reverse=True)[:8]:
            print("    %8.0f ms  %s" % (ms, what))


def linear_histogram(values: list[float], hi: float, buckets: int = 20,
                     width: int = 40) -> None:
    """Even buckets, unlike histogram() above -- and for the opposite reason.

    The log buckets exist to show how far a tail reaches.  This one exists to
    show a *shape*: if a wait is nothing but "time until the next poll", the
    waits are spread evenly between zero and one poll interval, and evenness
    is only visible with even buckets.
    """
    if not values:
        return
    step: float = hi / buckets
    # The last bucket collects everything over hi.
    counts: list[int] = [0] * (buckets + 1)
    for v in values:
        counts[min(buckets, int(v / step))] += 1
    top: int = max(counts) or 1
    for i, n in enumerate(counts):
        if i == buckets:
            label: str = " >%5.0f" % hi
        else:
            label = "%3.0f-%3.0f" % (i * step, (i + 1) * step)
        bar: str = "#" * int(round(n * width / top)) if n else ""
        print("    %9s ms  %-*s %4d" % (label, width, bar, n))


def digest_header(host: str, path: str, user: str, password: str,
                  method: str = "GET") -> str | None:
    """An Authorization header for raw-socket requests.

    The challenge is collected once and its nonce reused.  That is legal, and
    this WebServer permits it: it compares the nonce against the one it last
    issued and folds the client's counter into the hash without checking that
    the counter ever advances.  Reusing it keeps the measurement to one round
    trip per request, which is the whole point of measuring.

    The catch is "the one it last issued": every 401 mints a fresh nonce, so
    any unauthenticated request in between silently invalidates the header
    this returns -- and because the failure is itself a 401, it never
    recovers.  Build the header last, immediately before it is used.
    """
    try:
        urllib.request.urlopen("http://%s%s" % (host, path), timeout=6)
        return None  # no challenge, so the board is not asking for one
    except urllib.error.HTTPError as e:
        if e.code != 401:
            return None
        challenge: str = e.headers.get("WWW-Authenticate", "")
    except Exception:
        return None

    def param(name: str) -> str:
        m: re.Match[str] | None = re.search(
            r'%s="([^"]*)"' % name, challenge)
        return m.group(1) if m else ""

    realm, nonce, opaque = param("realm"), param("nonce"), param("opaque")
    if not (realm and nonce and opaque):
        return None

    def md5(s: str) -> str:
        return hashlib.md5(s.encode("utf-8")).hexdigest()

    cnonce, nc = "0a4f113b", "00000001"
    ha1: str = md5("%s:%s:%s" % (user, realm, password))
    ha2: str = md5("%s:%s" % (method, path))
    resp: str = md5(":".join([ha1, nonce, nc, cnonce, "auth", ha2]))
    return ('Digest username="%s", realm="%s", nonce="%s", uri="%s", '
            'qop=auth, nc=%s, cnonce="%s", response="%s", opaque="%s"'
            % (user, realm, nonce, path, nc, cnonce, resp, opaque))


def decompose(res: Result, api: Api, n: int, host: str, path: str,
              user: str | None, password: str | None,
              token: str | None) -> int:
    """Split a request into handshake, wait, and transfer.

    Written to settle an argument the ordinary timings could not.  A trivial
    request takes about 50 ms, of which roughly 35 had been attributed to
    waiting for the render loop to call handleClient() -- but the polls are
    one frame apart, so an evenly-spread wait should average half a frame,
    about 16 ms.  Twice the expected figure wants an explanation, and the two
    candidates are distinguishable by shape:

      one poll     the wait is spread evenly from 0 to one frame
      two polls    evenly from 0 to two frames, and the mean doubles
      the network  not spread at all, but piled up away from zero, and
                   the handshake -- measured here separately -- is large

    urllib cannot see the seam, because it hands back one number for the
    whole exchange.  A raw socket can: connect() is the handshake alone, and
    the first byte back cannot arrive until the board has been round the
    render loop.
    """
    ip: str = socket.gethostbyname(host.split(":")[0])

    st: Json = api.json("/state")
    fps: Any = 0
    if st:
        fps = (st.get("system") or {}).get("fps") or 0
    frame_ms: float = 1000.0 / fps if fps else 31.0
    print("measuring %d requests to %s" % (n, path))
    print("the board reports %s fps, so one frame is %.1f ms"
          % (fps or "no", frame_ms))
    if not fps:
        print("  (no frame rate reported -- assuming %.0f ms)" % frame_ms)

    # Last thing before the loop: a challenge is only good until the next
    # one is issued, and everything above could have provoked one.
    auth: str | None = None
    if token:
        auth = "Bearer " + token
    elif user and password:
        auth = digest_header(host, path, user, password)

    req: str = ("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
           % (path, host))
    if auth:
        req += "Authorization: %s\r\n" % auth
    req += "\r\n"
    blob: bytes = req.encode("ascii")

    connects: list[float] = []
    waits: list[float] = []
    transfers: list[float] = []
    codes: dict[int, int] = {}

    for i in range(n):
        s: socket.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10.0)
        try:
            t0: float = time.perf_counter()
            s.connect((ip, 80))
            t1: float = time.perf_counter()
            s.sendall(blob)
            first: bytes = s.recv(4096)
            t2: float = time.perf_counter()
            if not first:
                continue
            body: bytes = first
            while True:
                chunk: bytes = s.recv(4096)
                if not chunk:
                    break
                body += chunk
            t3: float = time.perf_counter()
        except Exception as e:
            print("  request %d failed: %s" % (i + 1, e))
            continue
        finally:
            s.close()

        try:
            code: int = int(body.split(b" ", 2)[1])
        except Exception:
            code = 0
        codes[code] = codes.get(code, 0) + 1
        connects.append((t1 - t0) * 1000.0)
        waits.append((t2 - t1) * 1000.0)
        transfers.append((t3 - t2) * 1000.0)

    if not waits:
        print("no request completed")
        return 1

    if list(codes) != [200]:
        print("responses: %s" % codes)
        if 401 in codes:
            print("  401 -- pass --user and --password, or measure an "
                  "AUTH_HTTP=0 build")
            return 1

    def row(name: str, vals: list[float]) -> None:
        print("  %-22s %7.1f %8.1f %8.1f %8.1f"
              % (name, percentile(vals, 50), percentile(vals, 90),
                 percentile(vals, 99), max(vals)))

    totals: list[float] = [
        c + w + t for c, w, t in zip(connects, waits, transfers)]
    print()
    print("  %d requests            median      p90      p99      max"
          % len(waits))
    row("handshake", connects)
    row("wait + serve", waits)
    row("rest of transfer", transfers)
    row("total", totals)

    mean: float = sum(waits) / len(waits)
    print()
    print("  wait + serve, against one frame of %.1f ms" % frame_ms)
    linear_histogram(waits, frame_ms * 2)
    print()
    print("  mean wait %.1f ms; half a frame is %.1f, a whole frame %.1f"
          % (mean, frame_ms / 2, frame_ms))

    # The shape is the answer.  Evenly spread up to one frame and the polling
    # interval explains everything; spread to two and something costs an
    # extra trip round the loop; piled up above the frame and the delay is
    # not the render loop at all.
    over: int = sum(1 for w in waits if w > frame_ms * 1.1)
    if over > len(waits) * 0.6:
        verdict: str = (
            "most waits exceed a whole frame, so the render loop is "
                   "not what they are waiting for")
    elif over > len(waits) * 0.2:
        verdict = ("a fifth or more spill past one frame -- some requests "
                   "are taking a second trip round the loop")
    else:
        verdict = ("the waits fit inside one frame, so the polling interval "
                   "accounts for them")
    print("  %s" % verdict)
    print()
    print("  handshake median %.1f ms is pure network: lwIP answers a SYN "
          % percentile(connects, 50))
    print("  from its own task, so the render loop cannot delay it.  Any "
          "part of")
    print("  the total above wait + handshake is the reply itself going out.")

    # A slow handshake is the one delay the firmware cannot be blamed for:
    # the connection is not the application's yet.  So if the outliers show up
    # here too, they are the link, and the round numbers say so -- a lost
    # segment waits out a retransmission timer, and those come in steps.
    slow: list[float] = sorted((c for c in connects if c > 100), reverse=True)
    if slow:
        print()
        print("  %d handshakes over 100 ms, before the board's own code saw "
              "the connection:" % len(slow))
        print("    " + ", ".join("%.0f" % c for c in slow[:12]))
        print("  Round figures near 250, 500 or 1000 ms are retransmission")
        print("  timers, which means lost packets rather than a slow board.")
    return 0


# --------------------------------------------------------------------- driver --

def main(argv: list[str]) -> int:
    ap: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="frank.local",
                    help="name or address of the board (default frank.local)")
    ap.add_argument("--user", help="digest username, for AUTH_HTTP builds")
    ap.add_argument("--password", help="digest password")
    ap.add_argument("--token", help="bearer token, for AUTH_TOKEN builds")
    ap.add_argument("--settings", action="store_true",
                    help="also test save/forget, which write flash")
    ap.add_argument("--credentials", action="store_true",
                    help="also change a password and change it back")
    ap.add_argument("--eye-file", metavar="FILE",
                    help="also upload FILE to the eye slot, remove it and put "
                         "it back; writes flash, and leaves FILE loaded")
    ap.add_argument("--restart", action="store_true",
                    help="also restart the board, and move the CPU speed "
                         "across a restart and back; writes flash")
    ap.add_argument("--wifi", action="store_true",
                    help="also test the wifi endpoints (read-only parts)")
    ap.add_argument("--latency", type=int, metavar="N",
                    help="skip the tests; time N requests per endpoint instead")
    ap.add_argument("--decompose", type=int, metavar="N",
                    help="split N requests into handshake, wait and transfer")
    ap.add_argument("--path", default="/api/v1/info",
                    help="what --decompose asks for (use a small reply)")
    ap.add_argument("--timeout", type=float, default=10.0)
    args: argparse.Namespace = ap.parse_args(argv[1:])

    res: Result = Result()
    api: Api = Api(args.host, res, args.user, args.password, args.token,
              args.timeout)

    if args.latency:
        return measure(res, api, args.latency)

    if args.decompose:
        return decompose(res, api, args.decompose, args.host, args.path,
                         args.user, args.password, args.token)

    print("testing http://%s/api/v1" % args.host)
    started: float = time.time()

    info: Json = test_info(res, api)
    if not info:
        print("\nthe board did not answer /api/v1/info -- is it up, and does "
              "it need a credential?")
        return 1

    start: Json = test_state(res, api)
    if not start:
        return 1

    test_page(res, api)
    test_eyes(res, api)
    test_eye_slot(res, api, args.eye_file)
    test_validation(res, api, start)
    test_dim(res, api, start)
    test_gaze(res, api)
    test_gaze_burst(res, api)
    test_dilate(res, api)
    test_toggles(res, api, start)
    test_cpu(res, api)
    test_clock(res, api, start)
    test_time(res, api)
    test_ntp(res, api)
    test_rtc(res, api, info)
    test_netinfo(res, api)
    test_actions(res, api)
    test_errors(res, api)
    test_request_gate(res, api, args.host)
    test_cors(res, api, info)
    test_auth(res, api, info, args.host)
    test_credentials(res, api, info, args)
    test_sleep(res, api)
    if args.wifi:
        test_wifi(res, api)
    else:
        res.heading("wifi")
        res.skip("the wifi endpoints", "pass --wifi to include them")
    if args.settings:
        test_settings(res, api)
    else:
        res.heading("saving")
        res.skip("save and forget", "pass --settings to include them")
    if args.restart:
        test_restart(res, api)
    else:
        res.heading("restart")
        res.skip("restarting, and the CPU speed across it",
                 "pass --restart to include them")

    print("\n== putting the board back ==")
    restore(api, start)
    res.ok("restored", "eye, gaze, width, pupil, swap, flip, brightness, clock")

    elapsed: float = time.time() - started
    print()
    print("%d passed, %d failed, %d skipped in %.0fs"
          % (res.passed, len(res.failed), len(res.skipped), elapsed))
    report_latency(res)
    if res.failed:
        print()
        for f in res.failed:
            print("  FAILED  " + f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
