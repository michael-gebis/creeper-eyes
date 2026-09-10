// The control page, as one string in flash.
//
// Kept in PROGMEM rather than assembled per request: the old page was built
// into a String on every load, which cost RAM and meant the markup and the
// device state were tangled together in C++.  This one is static, and every
// value on it comes from /api/v1/state at runtime.
//
// It is deliberately dependency-free -- no framework, no fonts, no CDN.  A
// prop on a home network should not need the internet to render its own
// control panel, and the CSP on an offline device would block it anyway.

#ifndef PAGE_H
#define PAGE_H

#include <pgmspace.h>

static const char CONTROL_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang=en><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>frank</title>
<style>
:root{--bg:#14161a;--card:#1d2026;--line:#2a2f38;--ink:#e6e8eb;--dim:#8b93a1;--accent:#6ba3d6}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--ink);
     font:14px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}
h1{font-size:20px;margin:0 0 2px}
h1 small{font-weight:400;color:var(--dim);font-size:13px}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));
      margin-top:14px;max-width:1000px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px 14px}
.card h2{font-size:12px;letter-spacing:.08em;text-transform:uppercase;
         color:var(--dim);margin:0 0 10px;font-weight:600}
button,select{font:inherit;background:#2a2f38;color:var(--ink);border:1px solid #363c47;
              border-radius:6px;padding:6px 10px;cursor:pointer}
button:hover{background:#333944}
button.on{background:var(--accent);border-color:var(--accent);color:#0d1117}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin-bottom:8px}
.row:last-child{margin-bottom:0}
label{color:var(--dim);min-width:52px}
input[type=range]{flex:1;min-width:90px;accent-color:var(--accent)}
kbd{font:12px ui-monospace,Menlo,Consolas,monospace;color:var(--dim)}
table{width:100%;border-collapse:collapse;font:12px ui-monospace,Menlo,Consolas,monospace}
td{padding:2px 0;vertical-align:top}
td:first-child{color:var(--dim);width:6.5em}
td.v{word-break:break-all}
#pad{width:100%;aspect-ratio:1;background:#12141a;border:1px solid var(--line);
     border-radius:8px;position:relative;cursor:crosshair;touch-action:none}
#dot{position:absolute;width:12px;height:12px;margin:-6px 0 0 -6px;border-radius:50%;
     background:var(--accent);pointer-events:none}
#err{color:#e8705f;min-height:1.2em;font-size:12px;margin-top:8px}
.dim{color:var(--dim)}
</style>

<h1>frank <small id=sub>connecting…</small></h1>

<div class=grid>
  <div class=card>
    <h2>Eye</h2>
    <div class=row><select id=eye></select><button id=eyeNext>next</button></div>
    <div class=row>
      <button data-act=blink>blink</button>
      <button data-act=startle>startle</button>
      <button data-act=splash>splash</button>
      <button id=netinfo>show address</button>
    </div>
  </div>

  <div class=card>
    <h2>Gaze</h2>
    <div id=pad><div id=dot style="left:50%;top:50%"></div></div>
    <div class=row style="margin-top:8px">
      <button id=gazeAuto>auto</button><span class="dim" id=gazeTxt></span>
    </div>
  </div>

  <div class=card>
    <h2>Pupil</h2>
    <div class=row>
      <label for=dil>width</label><input type=range id=dil min=0 max=100>
      <span id=dilTxt class=dim></span>
    </div>
    <div class=row>
      <button id=dilAuto>auto</button>
      <button id=pupil>pupil</button>
      <button id=swap>swap panels</button>
    </div>
  </div>

  <div class=card>
    <h2>Clock</h2>
    <div class=row>
      <button id=clkOn>on</button><button id=clkSec>seconds</button>
      <span id=clkTxt class=dim></span>
    </div>
    <div class=row><label for=tz>zone</label><select id=tz></select></div>
  </div>

  <div class=card>
    <h2>Device</h2>
    <table id=info></table>
  </div>

  <div class=card>
    <h2>Settings</h2>
    <div class=row>
      <button id=save>save</button><button id=forget>forget</button>
      <span id=dirty class=dim></span>
    </div>
    <p class=dim style="margin:8px 0 0;font-size:12px">
      Everything here is <kbd>/api/v1</kbd>. The serial console drives the
      same operations.</p>
  </div>
</div>

<div id=err></div>

<script>
const $ = s => document.querySelector(s);
let st = null, busy = false;

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

// Any control that changes something refreshes from the device afterwards,
// so the page always shows what the device actually did rather than what we
// asked for.
async function act(fn) {
  if (busy) return;
  busy = true; $('#err').textContent = '';
  try { await fn(); await refresh(); }
  catch (e) { $('#err').textContent = e.message; }
  finally { busy = false; }
}

function fillOnce(s) {
  if ($('#eye').options.length) return;
  api('/eyes').then(d => {
    $('#eye').innerHTML = d.designs
      .map(x => `<option value="${x.index}">${x.name}</option>`).join('');
    $('#eye').value = s.eye.index;
  });
  api('/tz').then(d => {
    $('#tz').innerHTML = '<option value="">(custom)</option>' +
      d.names.map(n => `<option value="${n}">${n}</option>`).join('');
  });
}

function render(s) {
  st = s;
  fillOnce(s);
  $('#sub').textContent = s.net.state === 'up'
    ? `${s.net.ipv4} · ${s.system.fps} fps` : s.net.state;

  $('#eye').value = s.eye.index;

  const g = s.gaze;
  $('#dot').style.left = (g.x / 1023 * 100) + '%';
  $('#dot').style.top = (flipY(g.y) / 1023 * 100) + '%';
  $('#gazeTxt').textContent = g.mode === 'auto' ? 'wandering' : `${g.x}, ${g.y}`;
  $('#gazeAuto').classList.toggle('on', g.mode === 'auto');

  if (document.activeElement !== $('#dil')) $('#dil').value = s.dilate.percent;
  $('#dilTxt').textContent = s.dilate.mode === 'auto' ? 'auto' : s.dilate.percent + '%';
  $('#dilAuto').classList.toggle('on', s.dilate.mode === 'auto');
  $('#pupil').classList.toggle('on', s.pupil.on);
  $('#swap').classList.toggle('on', s.swap.on);

  const showing = s.net.showingInfo;
  $('#netinfo').textContent = showing ? 'hide address' : 'show address';
  $('#netinfo').classList.toggle('on', showing);

  $('#clkOn').classList.toggle('on', s.clock.on);
  $('#clkSec').classList.toggle('on', s.clock.seconds);
  $('#clkTxt').textContent = s.clock.time +
    (s.net.timeSynced ? '' : ' (not synced)');

  $('#info').innerHTML = [
    ['host', s.net.mdns], ['mac', s.net.mac], ['ipv4', s.net.ipv4 || '—'],
    ['ipv6', s.net.ipv6 || '—'], ['wifi', s.net.ssid ? `${s.net.ssid} ${s.net.rssi} dBm` : '—'],
    ['tz', s.net.tz], ['heap', (s.system.freeHeap / 1024 | 0) + ' KB'],
    ['uptime', s.system.uptimeSeconds + ' s']
  ].map(([k, v]) => `<tr><td>${k}</td><td class=v>${v}</td></tr>`).join('');

  $('#dirty').textContent = s.system.settingsDirty ? 'unsaved changes' : 'saved';
}

async function refresh() { render(await api('/state')); }

// --- controls ---------------------------------------------------------------
$('#eye').onchange = e => act(() => api('/eye', 'PUT', {index: +e.target.value}));
$('#eyeNext').onclick = () => act(() => api('/eye', 'PUT', {next: true}));
document.querySelectorAll('[data-act]').forEach(b =>
  b.onclick = () => act(() => api('/action', 'POST', {action: b.dataset.act})));

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
$('#netinfo').onclick = () =>
  act(() => api('/netinfo', 'PUT', {on: !st.net.showingInfo}));

$('#gazeAuto').onclick = () => act(() => api('/gaze', 'PUT', {mode: 'auto'}));

$('#dil').onchange = e => act(() => api('/dilate', 'PUT', {percent: +e.target.value}));
$('#dilAuto').onclick = () => act(() => api('/dilate', 'PUT', {mode: 'auto'}));
$('#pupil').onclick = () => act(() => api('/pupil', 'PUT', {on: !st.pupil.on}));
$('#swap').onclick = () => act(() => api('/swap', 'PUT', {on: !st.swap.on}));

$('#clkOn').onclick = () => act(() => api('/clock', 'PUT', {on: !st.clock.on}));
$('#clkSec').onclick = () => act(() => api('/clock', 'PUT', {seconds: !st.clock.seconds}));
$('#tz').onchange = e => e.target.value &&
  act(() => api('/tz', 'PUT', {tz: e.target.value}));

$('#save').onclick = () => act(() => api('/settings', 'POST', {op: 'save'}));
$('#forget').onclick = () => act(() => api('/settings', 'POST', {op: 'forget'}));

// Polling rather than push: the synchronous web server cannot hold a
// connection open without stalling the render loop.  Every request costs a
// frame or two, so once a second is plenty.
refresh().catch(e => $('#err').textContent = e.message);
setInterval(() => { if (!busy) refresh().catch(() => {}); }, 1000);
</script>
)HTML";

#endif // PAGE_H
