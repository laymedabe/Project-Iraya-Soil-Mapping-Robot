# Project Iraya — Architecture & Safety Rationale (v1.00)

## 1. Why a layered control hierarchy

A single-computer design (Pi driving motors directly over GPIO) was rejected
for three reasons:

1. **Timing.** Python on a general-purpose OS cannot guarantee sub-10ms motor
   PWM or actuator position control under load (Flask requests, DB writes,
   Wi-Fi stack all compete for the same core).
2. **Network dependency for safety-critical actions.** If Wi-Fi drops while
   the actuator is inserting the NPK probe, a Pi-only design has no local
   fallback — the probe could remain extended while the robot receives no
   further commands.
3. **Fault isolation.** A Flask crash or Pi reboot should not leave motors
   in an undefined state.

The Arduino Mega therefore owns **all real-time hardware I/O and safety
logic**. The Pi is the supervisory layer: mission planning, data storage,
map generation, and the browser-facing API. It communicates with the Mega
only through a small, well-defined line-based command protocol.

## 2. Command protocol (Pi → Mega, UART, 115200 baud, `\n`-terminated)

| Command | Direction | Description |
|---|---|---|
| `DRIVE <FWD|BACK|LEFT|RIGHT> <speed 0-255>` | Pi → Mega | Sets drive state. Must be re-sent as a heartbeat (~150ms) while held. |
| `STOP` | Pi → Mega | Immediate motor cutoff. |
| `GOTO <lat> <lon>` | Pi → Mega | Informational — logged by Mega for status reporting; actual navigation/heading correction is computed on the Pi from GPS and sent as a stream of `DRIVE` commands, OR executed by Mega dead-reckoning in later iterations. |
| `SAMPLE` | Pi → Mega | Runs the LOWER → READ → RAISE sequence and reports `DATA`. |
| `ACK <cmd>` | Mega → Pi | Command received. |
| `STATUS <state>` | Mega → Pi | One of `MOVING, ALIGNED, LOWERING, READING, RAISED, IDLE, FAULT`. |
| `DATA N=.. P=.. K=.. MOIST=.. TEMP=.. EC=..` | Mega → Pi | Sensor reading, sent once per completed `SAMPLE`. |
| `FAULT <code>` | Mega → Pi | Reports a local safety trip (actuator stall, watchdog timeout, E-STOP). |

## 3. Safety layers (in order of authority)

1. **Physical E-STOP**, wired directly into the motor driver's enable line.
   Bypasses both the Mega and the Pi entirely. Non-negotiable for any robot
   operating near people.
2. **Arduino watchdog timeout** (400ms). If no fresh `DRIVE` command arrives,
   motors are forced to zero regardless of the last command received. This
   protects against Wi-Fi drops, browser crashes, or a hung Flask process.
3. **Flask `disconnect` handler.** If the Pi detects the browser's WebSocket
   has dropped, it proactively sends `STOP` — this is a *faster* stop than
   waiting for the Mega's own timeout, but is a convenience layer, not the
   safety guarantee (layer 2 is).
4. **Actuator stall/force protection**, implemented locally on the Mega
   (current sensing or max-time cutoff) so a jammed probe cannot over-drive
   the linear actuator even if the Pi never sends a follow-up command.

## 4. Data flow for a sampling session

1. Browser calls `POST /api/session/start` → Flask creates a `sessions` row,
   returns `session_id`.
2. For each waypoint: Flask sends `GOTO`, then `SAMPLE` over serial.
3. `serial_comm.py`'s background reader thread parses `STATUS` and `DATA`
   lines, pushes them onto an in-memory queue.
4. Flask's `/api/latest` endpoint (polled by the dashboard) drains the queue,
   writes `DATA` rows to `readings` in MariaDB, and returns the latest state
   as JSON.
5. `interpolation.py` recomputes the IDW grid whenever `/api/map` is
   requested, using all `readings` rows for the active session.

## 5. Chassis assumption

The chassis is 4WD with no visible steering linkage (all four wheels driven,
no separate steering servo/axle) — this is a **skid-steer** (differential
drive) configuration. `drive_control.cpp` mixes left/right motor pairs
accordingly. If the actual hardware uses Ackermann steering instead, only
`drive_control.cpp`'s `setDrive()` function needs to change — nothing else
in the architecture is affected.
