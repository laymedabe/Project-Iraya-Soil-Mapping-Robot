/* Project Iraya — dashboard frontend logic.
   Talks to the real Flask REST API (no simulated data here — the
   simulation lives server-side in serial_comm.py's SERIAL_SIMULATE mode
   when no Arduino Mega is physically attached). */

const bounds = window.IRAYA_FIELD_BOUNDS;
const el = id => document.getElementById(id);

let sessionId = null;
let waypointCount = 0;
let sampleIndex = 0;
let pollTimer = null;
let running = false;

/* ---------------- gauge ---------------- */
(function buildTicks(){
  const wrap = el('gaugeTicks');
  for(let mm=0; mm<=150; mm+=10){
    const t = document.createElement('div');
    t.className = 'gauge-tick' + (mm % 30 === 0 ? ' major' : '');
    t.style.bottom = (mm/150*100) + '%';
    wrap.appendChild(t);
  }
})();
function setGaugeDepth(mm){
  const pct = Math.max(0, Math.min(150, mm)) / 150 * 100;
  el('gaugeFill').style.height = pct + '%';
  el('gaugeProbe').style.bottom = 'calc(' + pct + '% + 6px)';
  el('depthVal').textContent = Math.round(mm);
}
function setStepUI(step){
  el('stepLabel').textContent = step;
  const live = ['MOVING','LOWERING','READING','ALIGNED'].includes(step);
  el('stepDot').className = 'dot ' + (live ? 'live' : 'idle');
  if(step === 'LOWERING' || step === 'READING') setGaugeDepth(130);
  else if(step === 'RAISED' || step === 'IDLE') setGaugeDepth(0);
}

/* ---------------- field map (canvas, IDW from server) ---------------- */
const canvas = el('fieldCanvas');
const ctx = canvas.getContext('2d');

let lastGridData = null;
let lastWaypoints = null;

function getCanvasDimensions(){
  const rect = canvas.parentElement.getBoundingClientRect();
  const w = Math.max(300, Math.floor(rect.width));
  const h = Math.max(200, Math.floor(rect.height || (rect.width * 560 / 900)));
  return { w, h, pad: Math.max(20, Math.floor(w * 0.05)) };
}

function syncCanvasSize(){
  const { w, h } = getCanvasDimensions();
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
    return true;
  }
  return false;
}

function latLonToXY(lat, lon, W, H, PAD){
  const x = PAD + (lon-bounds.lonMin)/(bounds.lonMax-bounds.lonMin) * (W-2*PAD);
  const y = PAD + (bounds.latMax-lat)/(bounds.latMax-bounds.latMin) * (H-2*PAD);
  return [x,y];
}
function colorForValue(v){
  const stops = [[0.0,[59,92,140]],[0.35,[93,160,184]],[0.65,[127,166,92]],[1.0,[201,162,61]],[1.4,[193,91,74]]];
  v = Math.max(0, Math.min(1.4, v));
  for(let i=0;i<stops.length-1;i++){
    const [v0,c0]=stops[i], [v1,c1]=stops[i+1];
    if(v>=v0 && v<=v1){ const t=(v-v0)/(v1-v0); return c0.map((c,idx)=>Math.round(c+(c1[idx]-c)*t)); }
  }
  return stops[stops.length-1][1];
}
function drawFieldFromGrid(gridResp, waypoints){
  if (gridResp !== undefined) lastGridData = gridResp;
  if (waypoints !== undefined) lastWaypoints = waypoints;

  syncCanvasSize();
  const W = canvas.width, H = canvas.height;
  const PAD = Math.max(20, Math.floor(W * 0.05));

  ctx.clearRect(0,0,W,H);
  ctx.fillStyle = '#0F0D09'; ctx.fillRect(0,0,W,H);

  const activeGrid = lastGridData;
  const activeWaypoints = lastWaypoints;

  if(activeGrid && activeGrid.grids && activeGrid.lats.length){
    const lats = activeGrid.lats, lons = activeGrid.lons;
    const res = lats.length;
    const cw = (W-2*PAD)/res, ch = (H-2*PAD)/res;
    const nGrid = activeGrid.grids.nitrogen, pGrid = activeGrid.grids.phosphorus, kGrid = activeGrid.grids.potassium;
    for(let i=0;i<res;i++){
      for(let j=0;j<res;j++){
        const composite = (nGrid[i][j]/80*0.4) + (pGrid[i][j]/45*0.3) + (kGrid[i][j]/220*0.3);
        const [r,g,b] = colorForValue(composite);
        ctx.fillStyle = `rgba(${r},${g},${b},0.85)`;
        ctx.fillRect(PAD+j*cw, PAD+i*ch, cw+1, ch+1);
      }
    }
  } else {
    ctx.fillStyle = '#17140E';
    ctx.fillRect(PAD, PAD, W-2*PAD, H-2*PAD);
  }

  // planned path
  if(activeWaypoints && activeWaypoints.length){
    ctx.strokeStyle = 'rgba(242,236,221,0.25)'; ctx.setLineDash([4,5]); ctx.lineWidth=1.5;
    ctx.beginPath();
    activeWaypoints.forEach((wp,idx)=>{
      const [x,y] = latLonToXY(parseFloat(wp.lat), parseFloat(wp.lon), W, H, PAD);
      if(idx===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
    });
    ctx.stroke(); ctx.setLineDash([]);

    activeWaypoints.forEach(wp=>{
      const [x,y] = latLonToXY(parseFloat(wp.lat), parseFloat(wp.lon), W, H, PAD);
      ctx.beginPath(); ctx.arc(x,y, Math.max(3, Math.floor(W/200)), 0, Math.PI*2);
      ctx.fillStyle = wp.visited ? 'rgba(242,236,221,0.9)' : 'rgba(242,236,221,0.25)';
      ctx.fill();
      if(wp.visited){ ctx.strokeStyle='#14120D'; ctx.lineWidth=1.5; ctx.stroke(); }
    });
  }

  ctx.fillStyle = 'rgba(185,176,154,0.8)';
  ctx.font = `${Math.max(9, Math.floor(W/75))}px "JetBrains Mono", monospace`;
  ctx.fillText(bounds.lonMin.toFixed(3), PAD-4, H-PAD+14);
  ctx.fillText(bounds.lonMax.toFixed(3), W-PAD-28, H-PAD+14);
}
drawFieldFromGrid(null, []);

let resizeTimeout;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimeout);
  resizeTimeout = setTimeout(() => {
    drawFieldFromGrid();
  }, 100);
});

/* ---------------- trend chart ---------------- */
const trendChart = new Chart(el('trendChart').getContext('2d'), {
  type:'line',
  data:{ labels:[], datasets:[
    {label:'N', data:[], borderColor:'#9DCB74', backgroundColor:'transparent', tension:0.35, pointRadius:2},
    {label:'P', data:[], borderColor:'#C97F3D', backgroundColor:'transparent', tension:0.35, pointRadius:2},
    {label:'K', data:[], borderColor:'#5DA0B8', backgroundColor:'transparent', tension:0.35, pointRadius:2},
  ]},
  options:{ responsive:true, maintainAspectRatio:false,
    plugins:{ legend:{ labels:{ color:'#B9B09A', font:{family:'JetBrains Mono', size:10} } } },
    scales:{ x:{ ticks:{color:'#7d745e', font:{family:'JetBrains Mono', size:9}}, grid:{color:'#3A3428'} },
             y:{ ticks:{color:'#7d745e', font:{family:'JetBrains Mono', size:9}}, grid:{color:'#3A3428'} } }
  }
});

function statusTag(n,p,k){
  const composite = n/80 + p/45 + k/220;
  if(composite < 0.75) return {label:'Low', cls:'low'};
  if(composite > 1.5) return {label:'High', cls:'high'};
  return {label:'OK', cls:'ok'};
}
function addLogRow(idx, r, tag){
  el('emptyLog').style.display = 'none';
  const tr = document.createElement('tr');
  const time = new Date().toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit'});
  tr.innerHTML = `<td>${idx}</td><td>${time}</td><td>${r.lat.toFixed(4)}, ${r.lon.toFixed(4)}</td>
    <td>${r.nitrogen.toFixed(1)}</td><td>${r.phosphorus.toFixed(1)}</td><td>${r.potassium.toFixed(1)}</td>
    <td><span class="tag ${tag.cls}">${tag.label}</span></td>`;
  el('logBody').prepend(tr);
}

/* ---------------- API calls ---------------- */
async function startSession(){
  const resp = await fetch('/api/session/start', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ field_name:'Demo Field',
      lat_min:bounds.latMin, lat_max:bounds.latMax, lon_min:bounds.lonMin, lon_max:bounds.lonMax })
  });
  const data = await resp.json();
  sessionId = data.session_id;
  waypointCount = data.waypoint_count;
  sampleIndex = 0;
  el('sampleCount').textContent = `0 / ${waypointCount}`;
  el('logSub').textContent = `· 0 of ${waypointCount} points collected`;
  running = true;
  requestNextSample();
  pollTimer = setInterval(pollLatest, 1500);
}

async function requestNextSample(){
  if(!running || !sessionId) return;
  const resp = await fetch(`/api/session/${sessionId}/sample`, {method:'POST'});
  const data = await resp.json();
  if(data.done){
    finishRun();
    return;
  }
  setStepUI('MOVING');
  el('gpsVal').textContent = `${data.lat.toFixed(4)}, ${data.lon.toFixed(4)}`;
}

async function pollLatest(){
  if(!sessionId) return;
  const resp = await fetch(`/api/session/${sessionId}/latest`);
  const data = await resp.json();

  el('linkDot').className = 'dot ' + (data.connected ? 'live' : 'warn');
  el('linkText').textContent = data.connected ? 'Connected' : 'Disconnected';
  setStepUI(data.mega_step);

  if(data.new_readings && data.new_readings.length){
    for(const r of data.new_readings){
      sampleIndex++;
      el('nVal').textContent = r.nitrogen.toFixed(1);
      el('pVal').textContent = r.phosphorus.toFixed(1);
      el('kVal').textContent = r.potassium.toFixed(1);
      el('nBar').style.width = Math.min(100, r.nitrogen/90*100) + '%';
      el('pBar').style.width = Math.min(100, r.phosphorus/50*100) + '%';
      el('kBar').style.width = Math.min(100, r.potassium/240*100) + '%';
      if(r.moisture !== undefined) el('moistVal').textContent = r.moisture.toFixed(1) + ' %';
      if(r.temperature !== undefined) el('tempVal').textContent = r.temperature.toFixed(1) + ' °C';
      if(r.ec !== undefined) el('ecVal').textContent = r.ec.toFixed(2) + ' dS/m';

      const tag = statusTag(r.nitrogen, r.phosphorus, r.potassium);
      addLogRow(sampleIndex, r, tag);

      trendChart.data.labels.push('#' + sampleIndex);
      trendChart.data.datasets[0].data.push(r.nitrogen);
      trendChart.data.datasets[1].data.push(r.phosphorus);
      trendChart.data.datasets[2].data.push(r.potassium);
      if(trendChart.data.labels.length > 12){
        trendChart.data.labels.shift();
        trendChart.data.datasets.forEach(d=>d.data.shift());
      }
      trendChart.update();
    }
    el('sampleCount').textContent = `${sampleIndex} / ${waypointCount}`;
    el('logSub').textContent = `· ${sampleIndex} of ${waypointCount} points collected`;

    // refresh map + advance to next waypoint
    refreshMap();
    if(sampleIndex >= waypointCount){ finishRun(); }
    else { requestNextSample(); }
  }
}

async function refreshMap(){
  const [mapResp, wpResp] = await Promise.all([
    fetch(`/api/session/${sessionId}/map`),
    fetch(`/api/session/${sessionId}/waypoints`),
  ]);
  const grid = await mapResp.json();
  const waypoints = await wpResp.json();
  drawFieldFromGrid(grid, waypoints);
}

function finishRun(){
  running = false;
  clearInterval(pollTimer);
  setStepUI('IDLE');
  el('startBtn').disabled = false;
  el('startBtn').textContent = 'Start Run';
  el('stopBtn').disabled = true;
  fetch(`/api/session/${sessionId}/stop`, {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({status:'completed'})
  });
}

el('startBtn').addEventListener('click', ()=>{
  el('startBtn').disabled = true;
  el('startBtn').textContent = 'Running…';
  el('stopBtn').disabled = false;
  startSession();
});

el('stopBtn').addEventListener('click', ()=>{
  if(sessionId){
    fetch(`/api/session/${sessionId}/stop`, {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({status:'aborted'})
    });
  }
  running = false;
  clearInterval(pollTimer);
  el('startBtn').disabled = false;
  el('startBtn').textContent = 'Start Run';
  el('stopBtn').disabled = true;
  setStepUI('IDLE');
});

/* ========================================================================
   MODE AWARENESS (Global Banner)
   ======================================================================== */
window.addEventListener('iraya:mode', (e) => {
  const { mode } = e.detail;
  const conflictBar = el('modeConflictBar');
  const dpadBtns = document.querySelectorAll('.dpad button[data-dir]:not([data-dir="STOP"])');
  const sampleBtn = el('takeSampleBtn');
  
  if (mode === 'auto') {
    if (conflictBar) conflictBar.style.display = 'flex';
    dpadBtns.forEach(b => b.disabled = true);
    if (sampleBtn) sampleBtn.disabled = true;
  } else {
    if (conflictBar) conflictBar.style.display = 'none';
    dpadBtns.forEach(b => b.disabled = false);
    if (sampleBtn && !sampleBtn.classList.contains('running')) {
      sampleBtn.disabled = false;
    }
  }
});

/* ========================================================================
   MANUAL DRIVE CONTROL
   ======================================================================== */
const socket = io();
let heartbeat = null;
let currentSpeed = 180;

const speedSlider = el('speedSlider');
if (speedSlider) {
  speedSlider.addEventListener('input', () => {
    currentSpeed = parseInt(speedSlider.value, 10);
    if (el('speedVal')) el('speedVal').textContent = currentSpeed;
  });
}

function startDrive(direction){
  socket.emit('drive', { direction, speed: currentSpeed });
  clearInterval(heartbeat);
  heartbeat = setInterval(() => {
    socket.emit('drive', { direction, speed: currentSpeed });
  }, 150);
}

function stopDrive(){
  clearInterval(heartbeat);
  socket.emit('drive', { direction: 'STOP', speed: 0 });
}

document.querySelectorAll('.dpad button[data-dir]').forEach(btn => {
  const dir = btn.dataset.dir;

  // Prevent right-click / long-press context menu on mobile
  btn.addEventListener('contextmenu', (e) => e.preventDefault());

  if(dir === 'STOP'){
    btn.addEventListener('click', (e) => { e.preventDefault(); stopDrive(); });
    btn.addEventListener('touchstart', (e) => { e.preventDefault(); stopDrive(); });
    return;
  }

  btn.addEventListener('mousedown', (e) => {
    e.preventDefault();
    btn.classList.add('active');
    startDrive(dir);
  });

  btn.addEventListener('touchstart', (e) => {
    if(e.cancelable) e.preventDefault();
    btn.classList.add('active');
    startDrive(dir);
  });

  ['mouseup','mouseleave','touchend','touchcancel'].forEach(evt => {
    btn.addEventListener(evt, (e) => {
      btn.classList.remove('active');
      stopDrive();
    });
  });
});

const emergencyStop = el('emergencyStop');
if (emergencyStop) {
  emergencyStop.addEventListener('click', () => {
    stopDrive();
    socket.emit('drive', { direction: 'STOP', speed: 0 }); 
  });
}

document.addEventListener('visibilitychange', () => {
  if(document.hidden) stopDrive();
});

/* ========================================================================
   MANUAL SOIL SAMPLE
   ======================================================================== */

function setSampleStepDot(n, state) {
  const dot = el('sDot' + n);
  if (dot) dot.className = 'sStep-dot ' + state;
}

function setSampleConnector(n, done) {
  const connectors = document.querySelectorAll('.sample-step-connector');
  if (connectors[n - 1]) {
    connectors[n - 1].className = 'sample-step-connector' + (done ? ' done' : '');
  }
}

function resetSampleUI() {
  [1, 2, 3].forEach(n => setSampleStepDot(n, ''));
  document.querySelectorAll('.sample-step-connector').forEach(c => c.className = 'sample-step-connector');
  if (el('sampleProgress')) el('sampleProgress').style.display = 'none';
  if (el('sampleError')) el('sampleError').style.display = 'none';
}

async function takeSample() {
  const btn = el('takeSampleBtn');
  const btnText = el('takeSampleBtnText');
  if (!btn) return;

  resetSampleUI();
  
  btn.classList.add('running');
  btn.disabled = true;
  if (btnText) btnText.textContent = 'Sampling in progress…';
  if (el('sampleProgress')) el('sampleProgress').style.display = 'flex';

  setSampleStepDot(1, 'active');

  const samplePromise = fetch('/api/manual/sample', { method: 'POST' });

  const step2Timer = setTimeout(() => {
    setSampleStepDot(1, 'done');
    setSampleConnector(1, true);
    setSampleStepDot(2, 'active');
  }, 600);

  const step3Timer = setTimeout(() => {
    setSampleStepDot(2, 'done');
    setSampleConnector(2, true);
    setSampleStepDot(3, 'active');
  }, 1200);

  let resp;
  try {
    resp = await samplePromise;
  } catch (netErr) {
    clearTimeout(step2Timer); clearTimeout(step3Timer);
    showSampleError('Network error: ' + netErr.message);
    resetSampleBtn(btn, btnText);
    return;
  }

  clearTimeout(step2Timer); clearTimeout(step3Timer);

  if (!resp.ok) {
    const body = await resp.json().catch(() => ({}));
    showSampleError(body.error || `Server error ${resp.status}`);
    resetSampleBtn(btn, btnText);
    return;
  }

  const data = await resp.json();
  const r = data.reading;

  setSampleStepDot(1, 'done'); setSampleConnector(1, true);
  setSampleStepDot(2, 'done'); setSampleConnector(2, true);
  setSampleStepDot(3, 'done');

  if (r) {
    r.lat = bounds.latMin; 
    r.lon = bounds.lonMin;
    const tag = statusTag(r.nitrogen || 0, r.phosphorus || 0, r.potassium || 0);
    
    // Log it with the actual status, using "—" for the index to show it's a manual spot check
    addLogRow("—", r, tag);
    
    // Reflect the manual reading in the Latest Reading and Telemetry panels
    if (el('nVal')) el('nVal').textContent = r.nitrogen.toFixed(1);
    if (el('pVal')) el('pVal').textContent = r.phosphorus.toFixed(1);
    if (el('kVal')) el('kVal').textContent = r.potassium.toFixed(1);
    if (el('nBar')) el('nBar').style.width = Math.min(100, r.nitrogen/90*100) + '%';
    if (el('pBar')) el('pBar').style.width = Math.min(100, r.phosphorus/50*100) + '%';
    if (el('kBar')) el('kBar').style.width = Math.min(100, r.potassium/240*100) + '%';
    
    if (r.moisture !== undefined && el('moistVal')) el('moistVal').textContent = r.moisture.toFixed(1) + ' %';
    if (r.temperature !== undefined && el('tempVal')) el('tempVal').textContent = r.temperature.toFixed(1) + ' °C';
    if (r.ec !== undefined && el('ecVal')) el('ecVal').textContent = r.ec.toFixed(2) + ' dS/m';
  }

  resetSampleBtn(btn, btnText);
}

function showSampleError(msg) {
  const errEl = el('sampleError');
  if (errEl) {
    errEl.textContent = '⚠ ' + msg;
    errEl.style.display = 'block';
  }
}

function resetSampleBtn(btn, btnText) {
  if (!btn) return;
  btn.classList.remove('running');
  btn.disabled = false;
  if (btnText) btnText.textContent = 'Take Soil Sample Now';
}

const sampleBtn = el('takeSampleBtn');
if (sampleBtn) {
  sampleBtn.addEventListener('click', takeSample);
}

const manualOpsToggle = el('manualOpsToggle');
const manualOpsContent = el('manualOpsContent');
const manualOpsChevron = el('manualOpsChevron');
if (manualOpsToggle && manualOpsContent && manualOpsChevron) {
  manualOpsToggle.addEventListener('click', () => {
    const isHidden = manualOpsContent.style.display === 'none';
    manualOpsContent.style.display = isHidden ? 'block' : 'none';
    manualOpsChevron.textContent = isHidden ? '▲' : '▼';
  });
}
