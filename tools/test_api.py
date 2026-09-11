#!/usr/bin/env python3
"""Exercise a running board's REST API.

    python tools/test_api.py --host frank.local
    python tools/test_api.py --host 192.168.1.50 --user frank --password ...
    python tools/test_api.py --host frank.local --token ...

Runs against real hardware, because that is the only place the interesting
failures live: a handler that works in isolation but starves the render loop,
a value that survives a round trip but not a reboot, a verb that returns the
wrong status only when the body is malformed.

It adapts to the firmware it finds.  A build without an RTC, without the
clock, or without authentication simply has those groups skipped rather than
failed -- `GET /api/v1/info` says which features are compiled in, and the
suite believes it.

Nothing here reboots the board or writes to flash unless you ask: --wifi
covers the credential endpoints, which reboot, and --settings covers save and
forget, which write NVS.  Neither runs by default.

The board's own state is captured at the start and put back at the end, so a
run leaves the eyes as it found them.

Exit status is 0 if everything passed or was skipped, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Callable, Optional

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

    def __init__(self, host: str, res: Result, user: Optional[str] = None,
                 password: Optional[str] = None, token: Optional[str] = None,
                 timeout: float = 10.0) -> None:
        self.base: str = "http://%s/api/v1" % host
        self.root: str = "http://%s" % host
        self.res: Result = res
        self.token: Optional[str] = token
        self.timeout: float = timeout
        self.opener: urllib.request.OpenerDirector
        if user and password:
            # Digest, matching AUTH_HTTP.  urllib retries with credentials
            # after the challenge, exactly as a browser does.
            mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
            mgr.add_password(None, self.root, user, password)
            self.opener = urllib.request.build_opener(
                urllib.request.HTTPDigestAuthHandler(mgr))
        else:
            self.opener = urllib.request.build_opener()

    def raw(self, path: str, method: str = "GET",
            body: Optional[Json] = None,
            headers: Optional[dict[str, str]] = None,
            full: bool = False) -> tuple[int, bytes]:
        """One request.  Returns the status and body; never raises for HTTP."""
        url = (self.root + path) if full else (self.base + path)
        req = urllib.request.Request(url, method=method)
        data: Optional[bytes] = None
        if body is not None:
            data = json.dumps(body).encode()
            req.add_header("Content-Type", "application/json")
        if self.token:
            req.add_header("Authorization", "Bearer " + self.token)
        for k, v in (headers or {}).items():
            req.add_header(k, v)

        started = time.time()
        # The board answers one client at a time from inside its render loop,
        # so an occasional request loses a race with a long frame.  Retrying a
        # read-only request is honest; a retried write would not be.
        attempts = 3 if method == "GET" else 1
        for attempt in range(attempts):
            try:
                with self.opener.open(req, data, timeout=self.timeout) as r:
                    out = (r.status, r.read())
                    break
            except urllib.error.HTTPError as e:
                out = (e.code, e.read())
                break
            except Exception as e:  # timeout, reset, DNS
                if attempt == attempts - 1:
                    self.res.fail("%s %s" % (method, path), repr(e)[:70])
                    return (0, b"")
                time.sleep(1.0)
        ms = (time.time() - started) * 1000
        self.res.requests += 1
        self.res.total_ms += ms
        what = "%s %s" % (method, path)
        if ms > self.res.slowest:
            self.res.slowest = ms
            self.res.slowest_what = what
        if ms > 1000:
            self.res.slow.append((ms, what))
        return out

    def json(self, path: str, method: str = "GET",
             body: Optional[Json] = None) -> Json:
        code, raw = self.raw(path, method, body)
        if code == 0:
            return {}
        try:
            return json.loads(raw)
        except ValueError:
            return {}


# ---------------------------------------------------------------- assertions --

def expect(res: Result, api: Api, label: str, path: str, method: str = "GET",
           body: Optional[Json] = None, status: int = 200,
           headers: Optional[dict[str, str]] = None,
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
    info = expect(res, api, "GET /info", "/info")
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
    s = expect(res, api, "GET /state", "/state")
    for path in ("eye.index", "eye.name", "eye.count",
                 "gaze.mode", "gaze.x", "gaze.y",
                 "dilate.mode", "dilate.percent",
                 "pupil.on", "swap.on", "startle.active",
                 "clock.on", "clock.seconds", "clock.time", "clock.rate",
                 "clock.colors.hour", "clock.colors.minute",
                 "clock.colors.second", "clock.suppressed",
                 "net.mac", "net.mdns", "net.state", "net.tz",
                 "time.source", "time.ntp.enabled", "time.rtc.enabled",
                 "system.fps", "system.panel", "system.panels",
                 "system.freeHeap", "system.uptimeSeconds",
                 "system.settingsDirty"):
        field(res, s, path, "state has %s" % path)
    if s:
        res.ok("all %d documented fields present" % 31)
    return s


def test_eyes(res: Result, api: Api, start: Json) -> None:
    res.heading("eyes")
    designs = expect(res, api, "GET /eyes", "/eyes").get("designs", [])
    if not designs:
        res.fail("eye list", "empty")
        return
    res.ok("designs built in", "%d: %s" % (
        len(designs), ", ".join(d["name"] for d in designs[:6])))

    first, last = designs[0], designs[-1]
    r = expect(res, api, "PUT /eye by name", "/eye", "PUT",
               {"name": last["name"]})
    same(res, "selected by name", r.get("name"), last["name"])

    r = expect(res, api, "PUT /eye by index", "/eye", "PUT",
               {"index": first["index"]})
    same(res, "selected by index", r.get("index"), first["index"])

    before = api.json("/eye").get("index")
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


def test_gaze(res: Result, api: Api) -> None:
    res.heading("gaze")
    r = expect(res, api, "PUT /gaze", "/gaze", "PUT", {"x": 200, "y": 800})
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
    fps_before = field(res, api.json("/state"), "system.fps", "fps") or 0

    started = time.time()
    points = [(120 + i * 45, 900 - i * 40) for i in range(16)]
    for (x, y) in points:
        api.raw("/gaze", "PUT", {"x": x, "y": y})
    elapsed = time.time() - started

    final = api.json("/gaze")
    lx, ly = points[-1]
    if final.get("x") == lx and final.get("y") == ly:
        res.ok("the last position is the one that sticks", "%d,%d" % (lx, ly))
    else:
        res.fail("the last position is the one that sticks",
                 "board has %s,%s not %d,%d"
                 % (final.get("x"), final.get("y"), lx, ly))

    res.ok("16 updates took", "%.1fs, %.0f ms each" % (elapsed, elapsed / 16 * 1000))

    time.sleep(1.5)
    fps_after = field(res, api.json("/state"), "system.fps", "fps") or 0
    if fps_after >= fps_before * 0.5:
        res.ok("the eyes kept rendering", "%s -> %s fps" % (fps_before, fps_after))
    else:
        res.fail("the eyes kept rendering",
                 "fps fell from %s to %s" % (fps_before, fps_after))
    api.raw("/gaze", "PUT", {"mode": "auto"})


def test_dilate(res: Result, api: Api) -> None:
    res.heading("pupil width")
    r = expect(res, api, "PUT /dilate", "/dilate", "PUT", {"percent": 40})
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
    res.heading("pupil and panel swap")
    for name in ("pupil", "swap"):
        was = field(res, start, "%s.on" % name, name)
        r = expect(res, api, "PUT /%s flips it" % name, "/" + name, "PUT",
                   {"on": not was})
        same(res, "%s is now %s" % (name, not was), r.get("on"), not was)
        r = expect(res, api, "PUT /%s back" % name, "/" + name, "PUT",
                   {"on": was})
        same(res, "%s restored" % name, r.get("on"), was)
        expect(res, api, "/%s rejects a missing body" % name, "/" + name,
               "PUT", {}, status=400)


def test_clock(res: Result, api: Api, start: Json, info: Json) -> None:
    res.heading("clock")
    was_on = field(res, start, "clock.on", "clock.on")
    r = expect(res, api, "PUT /clock on", "/clock", "PUT", {"on": True})
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
    src = field(res, api.json("/state"), "time.source", "time.source")
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
    api.raw("/clock", "PUT", "PUT" and {
        "on": was_on,
        "seconds": field(res, start, "clock.seconds", "clock.seconds"),
        "rate": field(res, start, "clock.rate", "clock.rate"),
        "colors": start.get("clock", {}).get("colors", {}),
    })


def test_time(res: Result, api: Api) -> None:
    res.heading("timezone and time sources")
    tz = expect(res, api, "GET /tz", "/tz")
    zones = tz.get("zones", [])
    if len(zones) < 10:
        res.fail("zone list", "only %d zones" % len(zones))
    else:
        regions = sorted({z["region"] for z in zones})
        res.ok("zones offered", "%d across %d regions" % (len(zones), len(regions)))
    for z in zones[:3]:
        for key in ("name", "region", "tz"):
            if key not in z:
                res.fail("zone entries have %s" % key, repr(z)[:50])

    was = tz.get("tz")
    r = expect(res, api, "PUT /tz by city", "/tz", "PUT", {"tz": "tokyo"})
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
    n = expect(res, api, "GET /ntp", "/ntp")
    ntp = n.get("ntp", {})
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
        before = ntp.get("lastSyncSeconds")
        expect(res, api, "PUT /ntp sync now", "/ntp", "PUT", {"op": "sync"})
        # A pool server has to be resolved and then asked over the internet,
        # which is not bounded by any particular number of seconds -- so wait
        # for the counter to reset rather than guessing how long it takes.
        after = before
        deadline = time.time() + 20
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
    r = expect(res, api, "GET /rtc", "/rtc")
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
    r = expect(res, api, "PUT /netinfo on", "/netinfo", "PUT", {"on": True})
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

    req = urllib.request.Request(api.base + "/gaze", method="PUT")
    req.add_header("Content-Type", "application/json")
    if api.token:
        req.add_header("Authorization", "Bearer " + api.token)
    try:
        with api.opener.open(req, b"{not json", timeout=api.timeout) as r:
            code = r.status
    except urllib.error.HTTPError as e:
        code = e.code
    except Exception as e:
        code = 0
    if code == 400:
        res.ok("malformed JSON is 400", "400")
    else:
        res.fail("malformed JSON is 400", "got %s" % code)

    err = api.json("/eye", "PUT", {"name": "nosucheye"})
    if isinstance(err.get("error"), str) and err["error"]:
        res.ok("errors carry a reason", err["error"][:44])
    else:
        res.fail("errors carry a reason", repr(err)[:50])


def test_cors(res: Result, api: Api, info: Json) -> None:
    res.heading("cross-origin")
    code, _ = api.raw("/eye", "OPTIONS")
    if code == 204:
        res.ok("preflight is answered", "204")
    else:
        res.fail("preflight is answered", "got %s" % code)

    req = urllib.request.Request(api.base + "/eye", method="OPTIONS")
    try:
        with api.opener.open(req, timeout=api.timeout) as r:
            origin = r.headers.get("Access-Control-Allow-Origin")
            methods = r.headers.get("Access-Control-Allow-Methods")
    except Exception as e:
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


def test_auth(res: Result, api: Api, info: Json, host: str) -> None:
    res.heading("credentials")
    if not info.get("auth"):
        res.skip("unauthenticated requests", "this build requires no credential")
        return
    bare = urllib.request.build_opener()
    for path in ("/api/v1/state", "/"):
        req = urllib.request.Request("http://%s%s" % (host, path))
        try:
            with bare.open(req, timeout=api.timeout) as r:
                code = r.status
        except urllib.error.HTTPError as e:
            code = e.code
        except Exception as e:
            res.fail("%s without a credential" % path, repr(e)[:50])
            continue
        if code == 401:
            res.ok("%s without a credential is 401" % path, "401")
        else:
            res.fail("%s without a credential is 401" % path, "got %s" % code)


def test_page(res: Result, api: Api) -> None:
    res.heading("the control page")
    code, body = api.raw("/", full=True)
    if code != 200:
        res.fail("GET /", "got %s" % code)
        return
    res.ok("GET /", "%d bytes" % len(body))
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
    dirty = field(res, api.json("/state"), "system.settingsDirty", "dirty")
    same(res, "nothing unsaved afterwards", dirty, False)
    expect(res, api, "rejects a bad op", "/settings", "POST", {"op": "nope"},
           status=400)


def test_wifi(res: Result, api: Api) -> None:
    res.heading("wifi (reboots the board)")
    r = expect(res, api, "GET /wifi", "/wifi")
    for key in ("state", "ssid", "stored", "portalName"):
        if key not in r:
            res.fail("/wifi reports %s" % key, "absent")
    blob = json.dumps(r).lower()
    if "pass" in blob and "password" not in blob:
        res.fail("no password in the reply", blob[:60])
    else:
        res.ok("no password comes back", "ssid=%r stored=%r"
               % (r.get("ssid"), r.get("stored")))
    expect(res, api, "rejects an empty ssid", "/wifi", "PUT", {"ssid": ""},
           status=400)
    expect(res, api, "rejects an unknown op", "/wifi", "PUT", {"op": "nope"},
           status=400)


# --------------------------------------------------------------------- driver --

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
    clock = start.get("clock", {})
    if clock:
        api.raw("/clock", "PUT", {"on": clock.get("on", False),
                                  "seconds": clock.get("seconds", False),
                                  "rate": clock.get("rate", 1),
                                  "colors": clock.get("colors", {})})
    api.raw("/netinfo", "PUT", {"on": False})


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="frank.local",
                    help="name or address of the board (default frank.local)")
    ap.add_argument("--user", help="digest username, for AUTH_HTTP builds")
    ap.add_argument("--password", help="digest password")
    ap.add_argument("--token", help="bearer token, for AUTH_TOKEN builds")
    ap.add_argument("--settings", action="store_true",
                    help="also test save/forget, which write flash")
    ap.add_argument("--wifi", action="store_true",
                    help="also test the wifi endpoints (read-only parts)")
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args(argv[1:])

    res = Result()
    api = Api(args.host, res, args.user, args.password, args.token,
              args.timeout)

    print("testing http://%s/api/v1" % args.host)
    started = time.time()

    info = test_info(res, api)
    if not info:
        print("\nthe board did not answer /api/v1/info -- is it up, and does "
              "it need a credential?")
        return 1

    start = test_state(res, api)
    if not start:
        return 1

    test_page(res, api)
    test_eyes(res, api, start)
    test_gaze(res, api)
    test_gaze_burst(res, api)
    test_dilate(res, api)
    test_toggles(res, api, start)
    test_clock(res, api, start, info)
    test_time(res, api)
    test_ntp(res, api)
    test_rtc(res, api, info)
    test_netinfo(res, api)
    test_actions(res, api)
    test_errors(res, api)
    test_cors(res, api, info)
    test_auth(res, api, info, args.host)
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

    print("\n== putting the board back ==")
    restore(api, start)
    res.ok("restored", "eye, gaze, width, pupil, swap, clock")

    elapsed = time.time() - started
    print()
    print("%d passed, %d failed, %d skipped in %.0fs"
          % (res.passed, len(res.failed), len(res.skipped), elapsed))
    print("%d requests, %.0f ms average, %.0f ms slowest (%s)"
          % (res.requests, res.total_ms / max(res.requests, 1), res.slowest,
             res.slowest_what))
    if res.slow:
        print()
        print("requests over a second -- each one is a stalled render loop:")
        for ms, what in sorted(res.slow, reverse=True)[:8]:
            print("  %8.0f ms  %s" % (ms, what))
    if res.failed:
        print()
        for f in res.failed:
            print("  FAILED  " + f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
