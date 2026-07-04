/* Project Iraya — manual drive control frontend.
   Sends a heartbeat 'drive' event every 150ms while a direction button is
   held. Releasing the button (mouseup/touchend/mouseleave) or losing the
   socket connection immediately sends STOP. The Arduino Mega's own
   watchdog (400ms) is the true safety backstop — see drive_control.cpp. */

const socket = io();
const el = id => document.getElementById(id);
let heartbeat = null;
let currentSpeed = 180;

const speedSlider = el('speedSlider');
speedSlider.addEventListener('input', () => {
  currentSpeed = parseInt(speedSlider.value, 10);
  el('speedVal').textContent = currentSpeed;
});

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
  if(dir === 'STOP'){
    btn.addEventListener('click', stopDrive);
    return;
  }
  btn.addEventListener('mousedown', () => { btn.classList.add('active'); startDrive(dir); });
  btn.addEventListener('touchstart', (e) => { e.preventDefault(); btn.classList.add('active'); startDrive(dir); });
  ['mouseup','mouseleave','touchend','touchcancel'].forEach(evt =>
    btn.addEventListener(evt, () => { btn.classList.remove('active'); stopDrive(); })
  );
});

el('emergencyStop').addEventListener('click', () => {
  stopDrive();
  socket.emit('drive', { direction: 'STOP', speed: 0 }); // send twice, belt-and-suspenders
});

socket.on('connect', () => {
  el('connBanner').textContent = 'Connected to Mega link';
  el('connBanner').className = 'conn-banner ok';
});
socket.on('disconnect', () => {
  el('connBanner').textContent = 'Disconnected — motors stopped';
  el('connBanner').className = 'conn-banner bad';
});
socket.on('drive_error', (data) => {
  el('connBanner').textContent = 'Drive error: ' + data.message;
  el('connBanner').className = 'conn-banner bad';
});

// Safety: if the page itself loses focus/visibility, stop driving.
document.addEventListener('visibilitychange', () => {
  if(document.hidden) stopDrive();
});
