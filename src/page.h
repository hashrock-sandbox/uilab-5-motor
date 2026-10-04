#pragma once

// スマホ向けの操作画面。ESP32 の "/" で配信する
const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>uilab motor</title>
<style>
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body { margin: 0; padding: 20px; font-family: system-ui, sans-serif; background: #111; color: #eee; user-select: none; }
  h1 { font-size: 18px; margin: 0 0 16px; color: #aaa; font-weight: 600; }
  section { background: #1c1c1c; border-radius: 16px; padding: 16px; margin-bottom: 16px; }
  h2 { font-size: 14px; margin: 0 0 12px; color: #888; font-weight: 600; }
  #dial { display: block; margin: 0 auto; touch-action: none; }
  .angle { text-align: center; font-size: 40px; font-variant-numeric: tabular-nums; margin: 4px 0 8px; }
  .row { display: flex; gap: 8px; flex-wrap: wrap; }
  button { flex: 1; min-width: 70px; padding: 14px 8px; font-size: 15px; border: 0; border-radius: 12px; background: #2c2c2c; color: #eee; }
  button.on { background: #f5a524; color: #111; }
  label { display: flex; justify-content: space-between; font-size: 13px; color: #aaa; margin: 12px 0 4px; }
  input[type=range] { width: 100%; accent-color: #f5a524; height: 32px; }
</style>
</head>
<body>
<h1>uilab motor</h1>

<section>
  <h2>サーボ</h2>
  <canvas id="dial" width="280" height="160"></canvas>
  <div class="angle"><span id="angle">90</span>°</div>
  <div class="row" id="modes">
    <button data-mode="manual" class="on">手動</button>
    <button data-mode="sweep">ワイパー</button>
    <button data-mode="wiggle">ぴこぴこ</button>
    <button data-mode="random">きまぐれ</button>
  </div>
  <label>速さ <span id="speedVal">180°/s</span></label>
  <input type="range" id="speed" min="20" max="600" value="180">
</section>

<section>
  <h2>DC モーター</h2>
  <label>出力 <span id="motorVal">0</span></label>
  <input type="range" id="motor" min="-255" max="255" value="0">
  <div class="row"><button id="motorStop">停止</button></div>
</section>

<script>
const $ = (id) => document.getElementById(id);
const dial = $('dial'), ctx = dial.getContext('2d');
const CX = 140, CY = 145, R = 120;
let shown = 90;

// 送信しすぎないよう、最後の値だけを一定間隔で送る
function throttled(path, interval = 60) {
  let pending = null, busy = false;
  const flush = () => {
    if (pending === null) { busy = false; return; }
    const v = pending; pending = null; busy = true;
    fetch(path + v).catch(() => {}).finally(() => setTimeout(flush, interval));
  };
  return (v) => { pending = v; if (!busy) flush(); };
}
const sendAngle = throttled('/servo?a=');
const sendSpeed = throttled('/speed?v=');
const sendMotor = throttled('/motor?v=');

function draw(angle) {
  ctx.clearRect(0, 0, 280, 160);
  ctx.lineWidth = 14; ctx.lineCap = 'round';
  ctx.strokeStyle = '#2c2c2c';
  ctx.beginPath(); ctx.arc(CX, CY, R, Math.PI, 0); ctx.stroke();
  // 0° を右、180° を左として半円に描く
  const rad = Math.PI - (angle / 180) * Math.PI;
  const x = CX + Math.cos(rad) * R, y = CY - Math.sin(rad) * R;
  ctx.strokeStyle = '#f5a524'; ctx.lineWidth = 6;
  ctx.beginPath(); ctx.moveTo(CX, CY); ctx.lineTo(x, y); ctx.stroke();
  ctx.fillStyle = '#f5a524';
  ctx.beginPath(); ctx.arc(x, y, 14, 0, Math.PI * 2); ctx.fill();
  ctx.beginPath(); ctx.arc(CX, CY, 8, 0, Math.PI * 2); ctx.fill();
}

function setModeButtons(mode) {
  document.querySelectorAll('#modes button').forEach((b) => b.classList.toggle('on', b.dataset.mode === mode));
}

// ダイヤルをドラッグして角度を指定する
function angleFromEvent(e) {
  const rect = dial.getBoundingClientRect();
  const x = (e.clientX - rect.left) * (280 / rect.width) - CX;
  const y = CY - (e.clientY - rect.top) * (160 / rect.height);
  const deg = Math.atan2(Math.max(y, 0), x) * 180 / Math.PI;
  return Math.round(180 - deg);
}
let dragging = false;
dial.addEventListener('pointerdown', (e) => { dragging = true; dial.setPointerCapture(e.pointerId); onDrag(e); });
dial.addEventListener('pointermove', (e) => { if (dragging) onDrag(e); });
dial.addEventListener('pointerup', () => { dragging = false; });
function onDrag(e) {
  const a = Math.min(180, Math.max(0, angleFromEvent(e)));
  setModeButtons('manual');
  sendAngle(a);
}

document.querySelectorAll('#modes button').forEach((b) => {
  b.addEventListener('click', () => { setModeButtons(b.dataset.mode); fetch('/mode?m=' + b.dataset.mode); });
});

$('speed').addEventListener('input', (e) => { $('speedVal').textContent = e.target.value + '°/s'; sendSpeed(e.target.value); });
$('motor').addEventListener('input', (e) => { $('motorVal').textContent = e.target.value; sendMotor(e.target.value); });
$('motorStop').addEventListener('click', () => { $('motor').value = 0; $('motorVal').textContent = 0; sendMotor(0); });

// ESP32 側の現在角度を取得してダイヤルに反映する
async function poll() {
  try {
    const s = await (await fetch('/state')).json();
    shown = s.angle;
    $('angle').textContent = Math.round(s.angle);
    if (!dragging) setModeButtons(s.mode);
  } catch (e) {}
  setTimeout(poll, 100);
}
function frame() { draw(shown); requestAnimationFrame(frame); }
poll(); frame();
</script>
</body>
</html>
)HTML";
