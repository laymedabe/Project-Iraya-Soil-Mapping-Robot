/* Project Iraya — dashboard frontend logic.
   Talks to the real Flask REST API (no simulated data here — the
   simulation lives server-side in serial_comm.py's SERIAL_SIMULATE mode
   when no Arduino Mega is physically attached). */

const bounds = window.IRAYA_FIELD_BOUNDS;
const el = id => document.getElementById(id);

let sampleIndex = 0;
let pollTimer = null;
let allReadings = [];
let latestRobotGps = null;

/* ---------------- gauge ---------------- */
(function buildTicks(){
  const wrap = el('gaugeTicks');
  if(!wrap) return;
  for(let mm=0; mm<=150; mm+=10){
    const t = document.createElement('div');
    t.className = 'gauge-tick' + (mm % 30 === 0 ? ' major' : '');
    t.style.bottom = (mm/150*100) + '%';
    wrap.appendChild(t);
  }
})();
function setGaugeDepth(mm){
  const pct = Math.max(0, Math.min(150, mm)) / 150 * 100;
  if(el('gaugeFill')) el('gaugeFill').style.height = pct + '%';
  if(el('gaugeProbe')) el('gaugeProbe').style.bottom = 'calc(' + pct + '% + 6px)';
  if(el('depthVal')) el('depthVal').textContent = Math.round(mm);
}
function setStepUI(step){
  if(el('stepLabel')) el('stepLabel').textContent = step;
  const live = ['MOVING','LOWERING','READING','RETRACTING','ALIGNED','AUTO_DRIVING','AUTO_SAMPLING'].includes(step);
  if(el('stepDot')) el('stepDot').className = 'dot ' + (live ? 'live' : 'idle');

  // Phase-based actuator gauge (relay-timed, no mm feedback)
  if(step === 'LOWERING') {
    setGaugeDepth(100);
    if(el('depthVal')) el('depthVal').textContent = 'Extending...';
  } else if(step === 'READING') {
    setGaugeDepth(130);
    if(el('depthVal')) el('depthVal').textContent = 'Holding / Reading';
  } else if(step === 'RETRACTING') {
    setGaugeDepth(60);
    if(el('depthVal')) el('depthVal').textContent = 'Retracting...';
  } else if(step === 'RAISED' || step === 'IDLE') {
    setGaugeDepth(0);
    if(el('depthVal')) el('depthVal').textContent = 'Standby';
  }
}

/* ---------------- field map (canvas, IDW interpolation) ---------------- */
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
  const x = PAD + (lon - bounds.lon_min) / (bounds.lon_max - bounds.lon_min) * (W - 2 * PAD);
  const y = PAD + (bounds.lat_max - lat) / (bounds.lat_max - bounds.lat_min) * (H - 2 * PAD);
  return [x, y];
}

function colorForValue(v){
  const stops = [
    [0.0,  [59, 92, 140]],  // Low - Slate Blue
    [0.35, [93, 160, 184]], // Low/Med - Cyan
    [0.65, [127, 166, 92]], // Optimal - Green
    [1.0,  [201, 162, 61]], // Medium/High - Yellow
    [1.4,  [193, 91, 74]]   // High - Coral Red
  ];
  v = Math.max(0, Math.min(1.4, v));
  for(let i = 0; i < stops.length - 1; i++){
    const [v0, c0] = stops[i], [v1, c1] = stops[i + 1];
    if(v >= v0 && v <= v1){
      const t = (v - v0) / (v1 - v0);
      return c0.map((c, idx) => Math.round(c + (c1[idx] - c) * t));
    }
  }
  return stops[stops.length - 1][1];
}

// Client-side helper to filter valid non-zero readings and deduplicate by coordinate
function getValidSamples(readings){
  if (!readings || !readings.length) return [];
  const nonZero = readings.filter(r => (
    (parseFloat(r.nitrogen) || 0) > 0 ||
    (parseFloat(r.phosphorus) || 0) > 0 ||
    (parseFloat(r.potassium) || 0) > 0
  ));
  const pool = nonZero.length ? nonZero : readings;
  const spatialMap = new Map();
  for (const r of pool) {
    const lat = parseFloat(r.lat);
    const lon = parseFloat(r.lon);
    if (isNaN(lat) || isNaN(lon)) continue;
    const key = `${lat.toFixed(4)},${lon.toFixed(4)}`;
    spatialMap.set(key, r);
  }
  return Array.from(spatialMap.values());
}

// Check if a server grid has active nutrient data (not all-zero)
function isGridActive(gridResp){
  if (!gridResp || !gridResp.grids || !gridResp.grids.nitrogen) return false;
  const nGrid = gridResp.grids.nitrogen;
  if (!nGrid.length || !nGrid[0].length) return false;
  for (let i = 0; i < nGrid.length; i++) {
    for (let j = 0; j < nGrid[i].length; j++) {
      if ((gridResp.grids.nitrogen[i][j] || 0) > 1.0 ||
          (gridResp.grids.phosphorus[i][j] || 0) > 1.0 ||
          (gridResp.grids.potassium[i][j] || 0) > 1.0) {
        return true;
      }
    }
  }
  return false;
}

// Client-side IDW fallback: ensures map reflects live high data even before server restarts
function computeClientGrid(samples, res = 40){
  if (!samples || !samples.length) return null;
  const latMin = bounds.lat_min, latMax = bounds.lat_max;
  const lonMin = bounds.lon_min, lonMax = bounds.lon_max;
  
  const lats = [];
  const lons = [];
  for (let i = 0; i < res; i++) {
    lats.push(latMax - (i / (res - 1)) * (latMax - latMin));
    lons.push(lonMin + (i / (res - 1)) * (lonMax - lonMin));
  }

  const grids = { nitrogen: [], phosphorus: [], potassium: [] };
  const epsilon = 1e-8;

  for (let i = 0; i < res; i++) {
    const rowN = [], rowP = [], rowK = [];
    const gLat = lats[i];
    for (let j = 0; j < res; j++) {
      const gLon = lons[j];
      let sumW = 0, sumWN = 0, sumWP = 0, sumWK = 0;
      for (const s of samples) {
        const sLat = parseFloat(s.lat);
        const sLon = parseFloat(s.lon);
        const d = Math.sqrt((sLat - gLat) ** 2 + (sLon - gLon) ** 2) + epsilon;
        const w = 1.0 / (d * d);
        sumW += w;
        sumWN += w * (parseFloat(s.nitrogen) || 0);
        sumWP += w * (parseFloat(s.phosphorus) || 0);
        sumWK += w * (parseFloat(s.potassium) || 0);
      }
      rowN.push(Number((sumWN / sumW).toFixed(2)));
      rowP.push(Number((sumWP / sumW).toFixed(2)));
      rowK.push(Number((sumWK / sumW).toFixed(2)));
    }
    grids.nitrogen.push(rowN);
    grids.phosphorus.push(rowP);
    grids.potassium.push(rowK);
  }

  return { lats, lons, grids };
}

function drawFieldFromGrid(gridResp, waypoints){
  if (gridResp !== undefined && gridResp !== null) lastGridData = gridResp;
  if (waypoints !== undefined && waypoints !== null) lastWaypoints = waypoints;

  syncCanvasSize();
  const W = canvas.width, H = canvas.height;
  const PAD = Math.max(20, Math.floor(W * 0.05));

  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = '#0F0D09';
  ctx.fillRect(0, 0, W, H);

  let activeGrid = lastGridData;
  const activeWaypoints = lastWaypoints;
  const validSamples = getValidSamples(allReadings);

  // If server grid is missing or all zeroes while we have valid NPK data, generate client-side IDW
  if (!isGridActive(activeGrid) && validSamples.length > 0) {
    activeGrid = computeClientGrid(validSamples, 40);
  }

  // Draw IDW Heatmap Cells
  if(activeGrid && activeGrid.grids && activeGrid.lats && activeGrid.lats.length){
    const lats = activeGrid.lats, lons = activeGrid.lons;
    const res = lats.length;
    const cw = (W - 2 * PAD) / res;
    const ch = (H - 2 * PAD) / res;
    const nGrid = activeGrid.grids.nitrogen, pGrid = activeGrid.grids.phosphorus, kGrid = activeGrid.grids.potassium;
    
    for(let i = 0; i < res; i++){
      for(let j = 0; j < res; j++){
        const nVal = (nGrid && nGrid[i]) ? nGrid[i][j] : 0;
        const pVal = (pGrid && pGrid[i]) ? pGrid[i][j] : 0;
        const kVal = (kGrid && kGrid[i]) ? kGrid[i][j] : 0;
        const composite = (nVal / 80 * 0.4) + (pVal / 45 * 0.3) + (kVal / 220 * 0.3);
        const [r, g, b] = colorForValue(composite);
        ctx.fillStyle = `rgba(${r},${g},${b},0.88)`;
        ctx.fillRect(Math.floor(PAD + j * cw), Math.floor(PAD + i * ch), Math.ceil(cw), Math.ceil(ch));
      }
    }

    // Subtle tactical grid mesh overlay
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.04)';
    ctx.lineWidth = 1;
    for(let k = 0; k <= 8; k++){
      const gx = PAD + k * ((W - 2 * PAD) / 8);
      const gy = PAD + k * ((H - 2 * PAD) / 8);
      ctx.beginPath(); ctx.moveTo(gx, PAD); ctx.lineTo(gx, H - PAD); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(PAD, gy); ctx.lineTo(W - PAD, gy); ctx.stroke();
    }
  } else {
    ctx.fillStyle = '#17140E';
    ctx.fillRect(PAD, PAD, W - 2 * PAD, H - 2 * PAD);
  }

  // Draw planned waypoints / path
  if(activeWaypoints && activeWaypoints.length){
    ctx.strokeStyle = 'rgba(242,236,221,0.25)';
    ctx.setLineDash([4, 5]);
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    activeWaypoints.forEach((wp, idx) => {
      const [x, y] = latLonToXY(parseFloat(wp.lat), parseFloat(wp.lon), W, H, PAD);
      if(idx === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();
    ctx.setLineDash([]);

    activeWaypoints.forEach(wp => {
      const [x, y] = latLonToXY(parseFloat(wp.lat), parseFloat(wp.lon), W, H, PAD);
      ctx.beginPath();
      ctx.arc(x, y, Math.max(3, Math.floor(W / 200)), 0, Math.PI * 2);
      ctx.fillStyle = wp.visited ? 'rgba(242,236,221,0.9)' : 'rgba(242,236,221,0.25)';
      ctx.fill();
      if(wp.visited){
        ctx.strokeStyle = '#14120D';
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }
    });
  }

  // Draw manual sample points with live NPK status colors
  if(validSamples && validSamples.length){
    validSamples.forEach((r, idx) => {
      const lat = parseFloat(r.lat);
      const lon = parseFloat(r.lon);
      if(isNaN(lat) || isNaN(lon)) return;
      const [x, y] = latLonToXY(lat, lon, W, H, PAD);

      const n = parseFloat(r.nitrogen || 0);
      const p = parseFloat(r.phosphorus || 0);
      const k = parseFloat(r.potassium || 0);
      const composite = (n / 80 * 0.4) + (p / 45 * 0.3) + (k / 220 * 0.3);
      const [pr, pg, pb] = colorForValue(composite);
      const isLatest = (idx === validSamples.length - 1);

      if (isLatest) {
        // Glowing halo around the latest sample
        ctx.beginPath();
        ctx.arc(x, y, Math.max(10, Math.floor(W / 90)), 0, Math.PI * 2);
        ctx.fillStyle = `rgba(${pr},${pg},${pb},0.35)`;
        ctx.fill();

        ctx.beginPath();
        ctx.arc(x, y, Math.max(8, Math.floor(W / 110)), 0, Math.PI * 2);
        ctx.strokeStyle = '#FFFFFF';
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }

      // Sample core dot
      ctx.beginPath();
      ctx.arc(x, y, Math.max(4.5, Math.floor(W / 180)), 0, Math.PI * 2);
      ctx.fillStyle = `rgb(${pr},${pg},${pb})`;
      ctx.fill();
      ctx.strokeStyle = '#0F0D09';
      ctx.lineWidth = 1.5;
      ctx.stroke();

      // Inner bright center for latest
      if (isLatest) {
        ctx.beginPath();
        ctx.arc(x, y, 2, 0, Math.PI * 2);
        ctx.fillStyle = '#FFFFFF';
        ctx.fill();

        // High-contrast label tag
        ctx.fillStyle = '#FFFFFF';
        ctx.font = `bold ${Math.max(10, Math.floor(W / 75))}px "JetBrains Mono", monospace`;
        ctx.fillText(`N:${n.toFixed(0)} P:${p.toFixed(0)} K:${k.toFixed(0)}`, x + 12, y - 4);
      }
    });
  }

  // Draw Live Robot GPS Position (if fix available)
  if(latestRobotGps && latestRobotGps.fix && !isNaN(latestRobotGps.lat) && !isNaN(latestRobotGps.lon)){
    const [rx, ry] = latLonToXY(latestRobotGps.lat, latestRobotGps.lon, W, H, PAD);
    ctx.beginPath();
    ctx.arc(rx, ry, 6, 0, Math.PI * 2);
    ctx.strokeStyle = '#64D2FF';
    ctx.lineWidth = 2;
    ctx.stroke();

    ctx.beginPath();
    ctx.arc(rx, ry, 2.5, 0, Math.PI * 2);
    ctx.fillStyle = '#64D2FF';
    ctx.fill();
  }

  // Boundary coordinates
  ctx.fillStyle = 'rgba(185,176,154,0.8)';
  ctx.font = `${Math.max(9, Math.floor(W / 75))}px "JetBrains Mono", monospace`;
  ctx.fillText(bounds.lon_min.toFixed(3), PAD - 4, H - PAD + 14);
  ctx.fillText(bounds.lon_max.toFixed(3), W - PAD - 28, H - PAD + 14);
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
  if(el('emptyLog')) el('emptyLog').style.display = 'none';
  const tr = document.createElement('tr');
  const time = new Date().toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit'});
  tr.innerHTML = `<td>${idx}</td><td>${time}</td><td>${r.lat.toFixed(4)}, ${r.lon.toFixed(4)}</td>
    <td>${r.nitrogen.toFixed(1)}</td><td>${r.phosphorus.toFixed(1)}</td><td>${r.potassium.toFixed(1)}</td>
    <td><span class="tag ${tag.cls}">${tag.label}</span></td>`;
  if(el('logBody')) el('logBody').prepend(tr);
}

function processReading(r) {
  const isDummyZero = (r.lat === 0 && r.lon === 0 && (!r.moisture || r.moisture === 0) && (!r.nitrogen || r.nitrogen === 0));
  if (isDummyZero && allReadings.length > 0) {
    return; // Ignore corrupt dummy zero readings from overriding UI
  }

  sampleIndex++;
  allReadings.push(r);
  if(el('nVal')) el('nVal').textContent = r.nitrogen.toFixed(1);
  if(el('pVal')) el('pVal').textContent = r.phosphorus.toFixed(1);
  if(el('kVal')) el('kVal').textContent = r.potassium.toFixed(1);
  if(el('nBar')) el('nBar').style.width = Math.min(100, r.nitrogen/90*100) + '%';
  if(el('pBar')) el('pBar').style.width = Math.min(100, r.phosphorus/50*100) + '%';
  if(el('kBar')) el('kBar').style.width = Math.min(100, r.potassium/240*100) + '%';
  if(r.moisture !== undefined && r.moisture !== null && el('moistVal')) el('moistVal').textContent = r.moisture.toFixed(1) + ' %';
  if(r.temperature !== undefined && r.temperature !== null && el('tempVal')) el('tempVal').textContent = r.temperature.toFixed(1) + ' °C';
  if(r.ec !== undefined && r.ec !== null && el('ecVal')) el('ecVal').textContent = r.ec.toFixed(2) + ' dS/m';
  if(r.ph !== undefined && r.ph !== null && el('phVal')) el('phVal').textContent = r.ph.toFixed(2);
  if(r.altitude !== undefined && r.altitude !== null && el('altVal')) el('altVal').textContent = r.altitude.toFixed(1) + ' m';
  if(r.satellites !== undefined && r.satellites !== null && el('satVal')) el('satVal').textContent = Math.round(r.satellites);
  if(r.hdop !== undefined && r.hdop !== null && el('hdopVal')) el('hdopVal').textContent = r.hdop.toFixed(2);

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
}

async function pollLatest(){
  try {
    const resp = await fetch('/api/telemetry');
    const data = await resp.json();

    if(el('linkDot')) el('linkDot').className = 'dot ' + (data.connected ? 'live' : 'warn');
    if(el('linkText')) el('linkText').textContent = data.connected ? 'Connected' : 'Disconnected';
    setStepUI(data.mega_step);

    if(data.gps && data.gps.fix){
        latestRobotGps = data.gps;
        if(el('gpsVal')) el('gpsVal').textContent = `${data.gps.lat.toFixed(5)}, ${data.gps.lon.toFixed(5)}`;
        if(el('altVal') && data.gps.alt !== undefined) el('altVal').textContent = data.gps.alt.toFixed(1) + ' m';
        if(el('satVal') && data.gps.sats !== undefined) el('satVal').textContent = Math.round(data.gps.sats);
        if(el('hdopVal') && data.gps.hdop !== undefined) el('hdopVal').textContent = data.gps.hdop.toFixed(2);
    } else {
        latestRobotGps = null;
        if(el('gpsVal')) el('gpsVal').textContent = 'No Fix';
    }

    if(data.new_readings && data.new_readings.length){
      for(const r of data.new_readings){
        processReading(r);
      }
      trendChart.update();
      if(el('sampleCount')) el('sampleCount').textContent = `${sampleIndex}`;
      if(el('logSub')) el('logSub').textContent = `· ${sampleIndex} manual points collected`;

      // refresh map
      refreshMap();
    }
  } catch(e) {
    console.error("Telemetry error", e);
    if (!window.hasAlerted) {
      alert("Telemetry UI Error: " + e.message);
      window.hasAlerted = true;
    }
  }
}

async function refreshMap(){
  try {
    const [mapResp, wpResp] = await Promise.all([
      fetch(`/api/map`),
      fetch(`/api/waypoints`),
    ]);
    const grid = await mapResp.json();
    const waypoints = await wpResp.json();
    drawFieldFromGrid(grid, waypoints);
  } catch (err) {
    console.error("Map refresh error", err);
    // Even if server map endpoint fails, draw from client readings
    drawFieldFromGrid(null, null);
  }
}

async function initDashboard() {
  try {
    const resp = await fetch('/api/readings');
    const readings = await resp.json();
    if (readings && readings.length) {
      for (const r of readings) {
        processReading(r);
      }
      trendChart.update();
      if(el('sampleCount')) el('sampleCount').textContent = `${sampleIndex}`;
      if(el('logSub')) el('logSub').textContent = `· ${sampleIndex} manual points collected`;
      refreshMap();
    }
  } catch(e) { 
    console.error(e); 
    alert("UI Error: " + e.message + " at " + e.stack);
  }
  
  // Start polling after historical load
  pollTimer = setInterval(pollLatest, 1500);
}

initDashboard();

const btnDownloadCSV = el('btnDownloadCSV');
if (btnDownloadCSV) {
  btnDownloadCSV.addEventListener('click', () => {
    window.location.href = '/api/export/csv';
  });
}

/* ── Manual Location Override Modal ── */
(function setupLocModal(){
  const overlay  = el('locModal');
  const btnOpen  = el('btnSetLocation');
  const btnClose = el('btnLocCancel');
  const btnApply = el('btnLocConfirm');
  const inLat    = el('inputLat');
  const inLon    = el('inputLon');
  const status   = el('locModalStatus');

  if(!overlay || !btnOpen) return;

  function showStatus(msg, isErr){
    status.textContent = msg;
    status.className = 'loc-modal-status ' + (isErr ? 'err' : 'ok');
    status.style.display = 'block';
  }

  btnOpen.addEventListener('click', () => {
    // Pre-fill with current GPS coords if available
    if(latestRobotGps && latestRobotGps.fix){
      inLat.value = latestRobotGps.lat.toFixed(6);
      inLon.value = latestRobotGps.lon.toFixed(6);
    }
    status.style.display = 'none';
    overlay.style.display = 'flex';
    inLat.focus();
  });

  function closeModal(){ overlay.style.display = 'none'; }
  btnClose.addEventListener('click', closeModal);
  overlay.addEventListener('click', e => { if(e.target === overlay) closeModal(); });

  btnApply.addEventListener('click', async () => {
    const lat = parseFloat(inLat.value);
    const lon = parseFloat(inLon.value);
    if(isNaN(lat) || isNaN(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180){
      showStatus('Invalid coordinates — check your values.', true);
      return;
    }

    btnApply.textContent = 'Applying…';
    btnApply.disabled = true;
    try {
      const resp = await fetch('/api/gps/override', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ lat, lon })
      });
      const result = await resp.json();
      if(resp.ok){
        // Update telemetry display immediately
        latestRobotGps = { lat, lon, fix: true, alt: latestRobotGps?.alt || 0,
                           sats: latestRobotGps?.sats || 0, hdop: latestRobotGps?.hdop || 0 };
        if(el('gpsVal')) el('gpsVal').textContent = `${lat.toFixed(5)}, ${lon.toFixed(5)} (Manual)`;
        showStatus(`✓ Location set: ${lat.toFixed(5)}, ${lon.toFixed(5)}`, false);
        setTimeout(closeModal, 1200);
      } else {
        showStatus('Error: ' + (result.error || 'Unknown error'), true);
      }
    } catch(e) {
      showStatus('Network error: ' + e.message, true);
    } finally {
      btnApply.textContent = 'Apply Location';
      btnApply.disabled = false;
    }
  });

  // Allow Enter key to confirm
  [inLat, inLon].forEach(inp => inp.addEventListener('keydown', e => {
    if(e.key === 'Enter') btnApply.click();
  }));
})();

