// The control page, as one string in flash.
//
// Kept in PROGMEM rather than assembled per request: the old page was built
// into a String on every load, which cost RAM and meant the markup and the
// device state were tangled together in C++.  This one is static, and every
// value on it comes from /api/v1/state at runtime.
//
// The cards are grouped by what happens to a setting when the power goes off
// -- persistent, session-only, or momentary -- because that is the first
// anyone asks of a control they have just moved, and a label on each card
// answers it without a legend to cross-reference.
//
// Deliberately dependency-free: no framework, no fonts, no CDN.  A prop on a
// home network should not need the internet to render its own control panel,
// and the CSP on an offline device would block it anyway.

#ifndef PAGE_H
#define PAGE_H

#include "favicon.h"
#include <pgmspace.h>

static const char CONTROL_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang=en><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<link rel=icon href=")HTML" FAVICON_URI R"HTML(">
<title>frank</title>
<style>
:root{--bg:#14161a;--card:#1d2026;--line:#2a2f38;--ink:#e6e8eb;--dim:#8b93a1;
      --accent:#6ba3d6;--warn:#e8a35f;--bad:#e8705f}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--ink);
     font:14px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}
h1{font-size:20px;margin:0 0 2px}
h1 small{font-weight:400;color:var(--dim);font-size:13px}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(270px,1fr));
      margin-top:14px;max-width:1180px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;
      padding:12px 14px;display:flex;flex-direction:column;gap:8px}
.card h2{font-size:12px;letter-spacing:.08em;text-transform:uppercase;
         color:var(--dim);margin:0;font-weight:600;display:flex;gap:8px;
         align-items:baseline;justify-content:space-between}
/* What survives a power cycle, said on the card rather than in a legend.
   The class stays .saved -- it pairs with the Settings card's save button,
   which is the thing that writes them. */
.tag{font-size:10px;letter-spacing:.04em;text-transform:none;padding:1px 6px;
     border-radius:9px;border:1px solid var(--line);color:var(--dim);
     font-weight:400;white-space:nowrap}
.tag.saved{border-color:#33506b;color:#8fb8dc}
button,select,input[type=text],input[type=password],input[type=time],
input[type=number]{font:inherit;background:#2a2f38;color:var(--ink);
     border:1px solid #363c47;border-radius:6px;padding:6px 10px}
button,select{cursor:pointer}
button:hover{background:#333944}
button.on{background:var(--accent);border-color:var(--accent);color:#0d1117}
button.danger:hover{background:#4a2b28;border-color:#7a4038}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
/* hidden is display:none, which a later display:flex would otherwise win. */
[hidden]{display:none!important}
.row>label{color:var(--dim);min-width:56px;font-size:13px}
.grow{flex:1;min-width:90px}
input[type=range]{flex:1;min-width:90px;accent-color:var(--accent)}
input[type=color]{width:38px;height:30px;padding:1px;background:none;
     border:1px solid #363c47;border-radius:6px;cursor:pointer}
kbd{font:12px ui-monospace,Menlo,Consolas,monospace;color:var(--dim)}
table{width:100%;border-collapse:collapse;font:12px ui-monospace,Menlo,Consolas,monospace}
td{padding:2px 0;vertical-align:top}
td:first-child{color:var(--dim);width:6.5em}
td.v{word-break:break-all}
#pad{width:100%;aspect-ratio:1;background:#12141a;border:1px solid var(--line);
     border-radius:8px;position:relative;cursor:crosshair;touch-action:none}
#dot{position:absolute;width:12px;height:12px;margin:-6px 0 0 -6px;border-radius:50%;
     background:var(--accent);pointer-events:none}
.note{color:var(--dim);font-size:12px;margin:0}
.note b{color:var(--warn);font-weight:600}
#err{color:var(--bad);min-height:1.2em;font-size:12px;margin-top:10px}
#banner{display:none;margin-top:12px;padding:10px 12px;border-radius:8px;
        background:#2b2418;border:1px solid #5a4526;color:var(--warn);font-size:13px}
.dim{color:var(--dim)}
a{color:var(--accent)}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;
     margin-right:7px;vertical-align:1px;background:var(--dim)}
.dot.good{background:#6fbf73}
.dot.warn{background:var(--warn)}
.dot.bad{background:var(--bad)}
.dot.off{background:transparent;border:1px solid var(--line)}
#tstat td:first-child{width:3.4em}
#tstat td.s{color:var(--ink);width:auto;font-family:inherit;font-size:13px}
#tstat td.d{color:var(--dim);font-size:11.5px}
hr{border:0;border-top:1px solid var(--line);margin:2px 0}
</style>

<h1>frank <small id=sub>connecting…</small></h1>

<div id=banner></div>

<div class=grid>
  <div class=card>
    <h2>Eye <span class="tag saved">persistent</span></h2>
    <div class=row><select id=eye class=grow></select><button id=eyeNext>next</button></div>
    <div class=row>
      <button id=pupil>pupil</button>
      <button id=swap>swap panels</button>
    </div>
    <p class=note>Swap trades which panel is Frank's left, for when the
      wiring came out the other way round.</p>
  </div>

  <div class=card>
    <h2>Gaze &amp; width <span class=tag>session only</span></h2>
    <div id=pad><div id=dot style="left:50%;top:50%"></div></div>
    <div class=row>
      <button id=gazeAuto>auto gaze</button><span class=dim id=gazeTxt></span>
    </div>
    <div class=row>
      <label for=dil>width</label><input type=range id=dil min=0 max=100>
      <span id=dilTxt class=dim></span>
    </div>
    <div class=row><button id=dilAuto>auto width</button></div>
  </div>

  <div class=card>
    <h2>Do something <span class=tag>momentary</span></h2>
    <div class=row>
      <button data-act=blink>blink</button>
      <button data-act=startle>startle</button>
      <button data-act=splash>splash</button>
    </div>
    <div class=row><button id=netinfo>show address</button></div>
    <p class=note>The address cards hold the panels for twelve seconds, or
      until you press the button again.</p>
  </div>

  <div class=card>
    <h2>Clock <span class="tag saved">persistent</span></h2>
    <div class=row>
      <button id=clkOn>on</button><button id=clkSec>seconds</button>
      <span id=clkTxt class=dim></span>
    </div>
    <div class=row>
      <span class=note id=clkWhy></span>
    </div>
    <div class=row>
      <label>hands</label>
      <input type=color id=cH title="hour hand"><input type=color id=cM title="minute hand">
      <input type=color id=cS title="second hand">
      <span class=dim id=panelNote></span>
    </div>
    <div class=row>
      <label for=rate>rate</label>
      <input type=number id=rate min=1 max=3600 class=grow>
      <button id=rateSet>set</button>
    </div>
    <p class=note>Rate is how many seconds the hands advance per real second,
      for checking the hands without waiting. No effect once NTP has answered.</p>
  </div>

  <div class=card>
    <h2>Time <span class=tag>not saved</span></h2>
    <div id=manual hidden>
      <div class=row>
        <input type=time id=tset step=1 class=grow><button id=timeSet>set</button>
      </div>
      <p class=note id=timeNote></p>
    </div>
    <table id=tstat></table>
    <div class=row>
      <button id=ntpOn>NTP</button>
      <button id=ntpSync>sync now</button>
    </div>
    <h2 style="margin-top:4px">Timezone <span class="tag saved">persistent</span></h2>
    <div class=row><select id=tz class=grow></select></div>
    <div class=row>
      <input type=text id=tzRaw class=grow placeholder="or a POSIX string">
      <button id=tzSet>set</button>
    </div>
  </div>

  <div class=card>
    <h2>Wi-Fi <span class=tag>stored by the radio</span></h2>
    <div class=row><span class=dim id=wifiTxt></span></div>
    <div class=row>
      <input type=text id=wSsid class=grow placeholder=network autocomplete=off>
    </div>
    <div class=row>
      <input type=password id=wPass class=grow placeholder=password autocomplete=off>
      <button id=wJoin>join</button>
    </div>
    <div class=row>
      <button id=wPortal>setup portal</button>
      <button id=wForget class=danger>forget</button>
    </div>
    <p class=note><b>Each of these reboots the board</b>, which takes a few
      seconds and may bring it back on a different address. The password is
      written, never read back.</p>
  </div>

  <div class=card>
    <h2>Device</h2>
    <table id=info></table>
    <p class=note id=v6note></p>
    <hr>
    <table id=about></table>
    <p class=note id=project></p>
  </div>

  <div class=card>
    <h2>Settings</h2>
    <div class=row>
      <button id=save>save</button><button id=forget class=danger>forget</button>
      <span id=dirty class=dim></span>
    </div>
    <p class=note>Save writes the cards marked <span class="tag saved">persistent</span>
      to flash. Forget clears them, so the next boot starts from the build-time
      defaults. Wi-Fi is stored separately, by the radio, and neither button
      touches it.</p>
    <p class=note>Everything here is <kbd>/api/v1</kbd>. The serial console
      drives the same operations.</p>
  </div>
</div>

<div id=err></div>

<script>
const $ = s => document.querySelector(s);
let st = null, busy = false, halted = false;

async function api(path, method, body) {
  const o = {method: method || 'GET'};
  if (body !== undefined) {
    o.headers = {'Content-Type': 'application/json'};
    o.body = JSON.stringify(body);
  }
  const r = await fetch('/api/v1' + path, o);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
  return j;
}

// Any control that changes something re-reads afterwards, so the page shows
// what the device actually did rather than what was asked of it.
async function act(fn) {
  if (busy || halted) return;
  busy = true; $('#err').textContent = '';
  try { await fn(); await refresh(); }
  catch (e) { $('#err').textContent = e.message; }
  finally { busy = false; }
}

// A field being edited must not be overwritten by the next poll.
const held = el => document.activeElement === el;

const hex = n => '#' + n.toLowerCase();
const unhex = v => v.replace('#', '').toUpperCase();

// The two lists that never change while the firmware is running.  Fetched
// once at startup and awaited one at a time: the device serves a single
// client from inside its render loop, so two requests in flight are two
// requests queued, and the second one is the one that times out.
// Fixed for the life of the build, so it is read once rather than polled.
async function fillInfo() {
  const i = await api('/info');
  $('#about').innerHTML = [
    ['version', i.version + (i.dirty ? ' (modified)' : '')],
    ['commit', i.commit],
    ['built', i.built],
    ['api', i.api]
  ].map(([k, v]) => `<tr><td>${k}</td><td class=v>${v}</td></tr>`).join('');
  // The commit is the one this firmware was built from, which is not
  // necessarily the tip of the branch it came from.
  $('#project').innerHTML =
    `<a href="${i.project}" target="_blank" rel="noreferrer">${i.project
      .replace('https://', '')}</a>`;
}

async function fillLists() {
  const eyes = await api('/eyes');
  $('#eye').innerHTML = eyes.designs
    .map(x => `<option value="${x.index}">${x.name}</option>`).join('');

  const tz = await api('/tz');
  // Grouped by region: sixty options in one flat list is a wall.
  let html = '', region = null;
  for (const z of tz.zones) {
    if (z.region !== region) {
      if (region !== null) html += '</optgroup>';
      region = z.region;
      html += `<optgroup label="${region}">`;
    }
    html += `<option value="${z.name}">${z.name.replace(/_/g, ' ')}</option>`;
  }
  $('#tz').innerHTML = html + '</optgroup>';
  // Nothing is preselected: the device stores a POSIX string, and several
  // cities map to the same one, so there is no honest answer to "which of
  // these is it".
  $('#tz').selectedIndex = -1;
}

// One pass over a /state reply, writing every control on the page.  Cheap
// enough to run whole once a second, and running it whole is what keeps the
// page honest -- there is no incremental path that could drift out of step
// with what the device actually reports.
function render(s) {
  st = s;
  $('#sub').textContent = s.net.state === 'up'
    ? `${s.net.ipv4} · ${s.system.fps} fps` : s.net.state;

  $('#eye').value = s.eye.index;
  $('#pupil').classList.toggle('on', s.pupil.on);
  $('#swap').classList.toggle('on', s.swap.on);

  const g = s.gaze;
  $('#dot').style.left = (g.x / 1023 * 100) + '%';
  $('#dot').style.top = (flipY(g.y) / 1023 * 100) + '%';
  $('#gazeTxt').textContent = g.mode === 'auto' ? 'wandering' : `${g.x}, ${g.y}`;
  $('#gazeAuto').classList.toggle('on', g.mode === 'auto');

  if (!held($('#dil'))) $('#dil').value = s.dilate.percent;
  $('#dilTxt').textContent = s.dilate.mode === 'auto' ? 'auto' : s.dilate.percent + '%';
  $('#dilAuto').classList.toggle('on', s.dilate.mode === 'auto');

  const showing = s.net.showingInfo;
  $('#netinfo').textContent = showing ? 'hide address' : 'show address';
  $('#netinfo').classList.toggle('on', showing);

  $('#clkOn').classList.toggle('on', s.clock.on);
  $('#clkSec').classList.toggle('on', s.clock.seconds);
  $('#clkTxt').textContent = s.clock.suppressed ? '' : s.clock.time;
  $('#clkWhy').innerHTML = s.clock.suppressed
    ? '<b>Hidden.</b> Nothing knows what time it is — no time server, no RTC, ' +
      'and it has not been set by hand. The face returns by itself as soon as ' +
      'something supplies a time.'
    : '';
  if (!held($('#cH'))) $('#cH').value = hex(s.clock.colors.hour);
  if (!held($('#cM'))) $('#cM').value = hex(s.clock.colors.minute);
  if (!held($('#cS'))) $('#cS').value = hex(s.clock.colors.second);
  if (!held($('#rate'))) $('#rate').value = s.clock.rate;
  $('#panelNote').textContent =
    s.system.panel === 'ssd1327' ? 'greyscale — shown as brightness' : '';

  renderTimeStatus(s.time);

  // Setting the time by hand is only worth offering when nothing is going to
  // overrule it.  With a time server in charge the field would accept a value
  // and then quietly have no effect, which is worse than not being there.
  const ntp = s.time.ntp;
  $('#ntpOn').classList.toggle('on', ntp.enabled);
  $('#ntpOn').textContent = ntp.enabled ? 'NTP on' : 'NTP off';
  $('#ntpSync').hidden = !ntp.enabled;
  $('#manual').hidden = ntp.enabled;

  $('#timeNote').innerHTML = s.time.rtc.present
    ? 'Stored in the battery-backed clock, so it survives a power cut.'
    : 'Kept until the power goes off. Fit an RTC and it is kept for good.';

  $('#wifiTxt').textContent = s.net.state === 'up'
    ? `on ${s.net.ssid}, ${s.net.rssi} dBm`
    : (s.net.state === 'portal' ? 'setup portal open' : 'not connected');

  const rows = [
    ['host', s.net.mdns], ['mac', s.net.mac], ['ipv4', s.net.ipv4 || '—']
  ];
  if (s.net.ipv6) rows.push(['ipv6', s.net.ipv6]);
  rows.push(
    ['wifi', s.net.ssid ? `${s.net.ssid} ${s.net.rssi} dBm` : '—'],
    ['tz', s.net.tz], ['panel', `${s.system.panel} ×${s.system.panels}`],
    ['heap', (s.system.freeHeap / 1024 | 0) + ' KB'],
    ['uptime', s.system.uptimeSeconds + ' s']);
  $('#info').innerHTML = rows
    .map(([k, v]) => `<tr><td>${k}</td><td class=v>${v}</td></tr>`).join('');

  // Only worth explaining when there is an address on screen that does not
  // work.  With IPv6 compiled out there is nothing to explain away.
  $('#v6note').innerHTML = (s.net.ipv6 && !s.net.ipv6Served)
    ? 'The IPv6 address answers pings but not HTTP: the ESP32 Arduino core ' +
      'binds an IPv4 socket only. Use the IPv4 address or <kbd>frank.local</kbd>.'
    : '';

  $('#dirty').textContent = s.system.settingsDirty ? 'unsaved changes' : 'saved';
}

// Where the time came from matters more than the time itself: a clock
// reading 3:47 is not worth much until you know whether that came off a time
// server, out of a battery-backed chip, or a counter that started at boot.
// Each row is a state, a colour, and the detail that justifies it.
const ago = n => n === undefined ? 'never'
  : n < 60 ? n + 's ago'
  : n < 3600 ? Math.round(n / 60) + ' min ago'
  : Math.round(n / 3600) + ' h ago';

function renderTimeStatus(t) {
  const SRC = {ntp: 'NTP', rtc: 'RTC', manual: 'set by hand',
               free: 'free-running'};

  const n = t.ntp;
  let ntp;
  if (!n.available) ntp = ['off', 'not built in', ''];
  else if (!n.enabled) ntp = ['off', 'switched off',
                              'the time already set is kept'];
  else if (!n.linkUp) ntp = ['bad', 'no network', 'nothing to ask'];
  else if (!n.running) ntp = ['bad', 'not started', ''];
  else if (!n.synced) ntp = ['warn', 'waiting for a reply', n.server];
  else ntp = ['good', 'synced ' + ago(n.lastSyncSeconds),
              `${n.server}, every ${Math.round(n.intervalSeconds / 3600)} h`];

  const r = t.rtc;
  // The DS3231 measures its own temperature to compensate its crystal, so a
  // reading costs nothing and doubles as proof the bus is working -- worth
  // showing even when the cell is flat and the time is not to be believed.
  const temp = r.temperatureC !== undefined
    ? `${r.temperatureC.toFixed(2)} °C` : '';
  const join = (a, b) => [a, b].filter(Boolean).join('  ·  ');

  let rtc;
  if (!r.enabled) rtc = ['off', 'not built in', 'build with -DRTC=1'];
  else if (!r.present) rtc = ['bad', 'no module found', 'check the wiring'];
  else if (!r.valid) rtc = ['warn', 'battery lost or never set',
                            join('set the time and it will be kept', temp)];
  else rtc = ['good', 'keeping time', join(r.utc, temp)];

  $('#tstat').innerHTML = [
    ['src', 'good', SRC[t.source] || t.source, ''],
    ['ntp', ntp[0], ntp[1], ntp[2]],
    ['rtc', rtc[0], rtc[1], rtc[2]]
  ].map(([k, cls, state, detail]) =>
    `<tr><td>${k}</td><td class=s><i class="dot ${cls}"></i>${state}` +
    (detail ? `<div class=d>${detail}</div>` : '') + `</td></tr>`).join('');
}

async function refresh() { render(await api('/state')); }

// --- eye --------------------------------------------------------------------
$('#eye').onchange = e => act(() => api('/eye', 'PUT', {index: +e.target.value}));
$('#eyeNext').onclick = () => act(() => api('/eye', 'PUT', {next: true}));
$('#pupil').onclick = () => act(() => api('/pupil', 'PUT', {on: !st.pupil.on}));
$('#swap').onclick = () => act(() => api('/swap', 'PUT', {on: !st.swap.on}));

// --- gaze and width ---------------------------------------------------------
// The device puts y=1023 at the top of the gaze range, the way a joystick
// does; the screen puts y=0 there.  Flip at this boundary rather than in the
// firmware, so `look` over serial and the API keep the convention they have
// always documented.
const flipY = y => 1023 - y;

const pad = $('#pad');
function aim(ev) {
  const r = pad.getBoundingClientRect();
  const c = v => Math.max(0, Math.min(1023, Math.round(v)));
  const x = c((ev.clientX - r.left) / r.width * 1023);
  const y = c((ev.clientY - r.top) / r.height * 1023);
  act(() => api('/gaze', 'PUT', {x: x, y: flipY(y)}));
}
pad.onpointerdown = e => { pad.setPointerCapture(e.pointerId); aim(e); };
pad.onpointermove = e => { if (e.buttons) aim(e); };
$('#gazeAuto').onclick = () => act(() => api('/gaze', 'PUT', {mode: 'auto'}));
$('#dil').onchange = e => act(() => api('/dilate', 'PUT', {percent: +e.target.value}));
$('#dilAuto').onclick = () => act(() => api('/dilate', 'PUT', {mode: 'auto'}));

// --- one-shots --------------------------------------------------------------
document.querySelectorAll('[data-act]').forEach(b =>
  b.onclick = () => act(() => api('/action', 'POST', {action: b.dataset.act})));
$('#netinfo').onclick = () =>
  act(() => api('/netinfo', 'PUT', {on: !st.net.showingInfo}));

// --- clock ------------------------------------------------------------------
$('#clkOn').onclick = () => act(() => api('/clock', 'PUT', {on: !st.clock.on}));
$('#clkSec').onclick = () => act(() => api('/clock', 'PUT', {seconds: !st.clock.seconds}));
const colour = (id, which) => $(id).onchange = e =>
  act(() => api('/clock', 'PUT', {colors: {[which]: unhex(e.target.value)}}));
colour('#cH', 'hour'); colour('#cM', 'minute'); colour('#cS', 'second');
$('#rateSet').onclick = () =>
  act(() => api('/clock', 'PUT', {rate: +$('#rate').value}));
// Restarts the client so it asks now instead of waiting out the interval.
// The answer lands asynchronously; the next poll shows it.
$('#ntpSync').onclick = () => act(() => api('/ntp', 'PUT', {op: 'sync'}));
$('#ntpOn').onclick = () =>
  act(() => api('/ntp', 'PUT', {enabled: !st.time.ntp.enabled}));

$('#timeSet').onclick = () => {
  const v = $('#tset').value;
  if (!v) { $('#err').textContent = 'pick a time first'; return; }
  act(() => api('/clock', 'PUT', {time: v}));
};

// --- timezone ---------------------------------------------------------------
$('#tz').onchange = e => e.target.value &&
  act(() => api('/tz', 'PUT', {tz: e.target.value}));
$('#tzSet').onclick = () => {
  const v = $('#tzRaw').value.trim();
  if (!v) { $('#err').textContent = 'type a POSIX string first'; return; }
  act(() => api('/tz', 'PUT', {tz: v}).then(() => { $('#tzRaw').value = ''; }));
};

// --- wifi -------------------------------------------------------------------
// Each of these drops the link this page is served over, so the poll stops
// and the page says what is happening rather than filling up with errors.
function reboot(fn, what) {
  if (!confirm(what + '\n\nThe board reboots and may come back on a ' +
               'different address.')) return;
  halted = true;
  $('#banner').style.display = 'block';
  $('#banner').textContent = 'Rebooting. Reload in a few seconds — try ' +
    'http://frank.local/ if the address has changed.';
  fn().catch(() => {}); // the reply may not outlive the radio
}

$('#wJoin').onclick = () => {
  const ssid = $('#wSsid').value.trim();
  if (!ssid) { $('#err').textContent = 'enter a network name'; return; }
  reboot(() => api('/wifi', 'PUT', {ssid: ssid, pass: $('#wPass').value}),
         'Join "' + ssid + '"?');
};
$('#wPortal').onclick = () =>
  reboot(() => api('/wifi', 'PUT', {op: 'portal'}),
         'Reboot into the setup portal?');
$('#wForget').onclick = () =>
  reboot(() => api('/wifi', 'PUT', {op: 'forget'}),
         'Forget the stored network?');

// --- settings ---------------------------------------------------------------
$('#save').onclick = () => act(() => api('/settings', 'POST', {op: 'save'}));
$('#forget').onclick = () => {
  if (!confirm('Clear the saved settings?\n\nThe next boot starts from the ' +
               'build-time defaults. Wi-Fi is not affected.')) return;
  act(() => api('/settings', 'POST', {op: 'forget'}));
};

// Polling rather than push: the synchronous web server cannot hold a
// connection open without stalling the render loop.
//
// The next poll is scheduled when the last one lands, rather than on a timer.
// setInterval fires whether or not the previous request has come back, and
// the device serves one client at a time from inside its render loop -- so a
// slow reply used to leave a second request outstanding, then a third, until
// the browser's connection pool was full and the page stopped responding
// altogether.  A chained timeout cannot stack by construction, however slow
// the device gets.
async function poll() {
  if (!halted && !busy) {
    try { await refresh(); $('#err').textContent = ''; }
    catch (e) { $('#err').textContent = 'lost contact — retrying'; }
  }
  if (!halted) setTimeout(poll, 1000);
}

(async () => {
  try {
    await refresh();
    await fillLists();
    await fillInfo();
  } catch (e) {
    $('#err').textContent = e.message;
  }
  poll();
})();
</script>
)HTML";

#endif // PAGE_H
