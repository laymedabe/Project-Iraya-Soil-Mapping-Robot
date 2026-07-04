/*
 * Project Iraya — Arduino Mega firmware
 * Real-time controller for drive motors, the linear actuator, and the
 * RS485 NPK sensor. Communicates with the Raspberry Pi over USB serial
 * using a simple line-based text protocol (see docs/ARCHITECTURE.md).
 *
 * Required libraries: none beyond the Arduino core (drive/actuator/NPK
 * drivers here are hand-rolled to avoid external dependency version drift).
 *
 * Wiring: see config.h — update pin assignments there before flashing.
 *
 * Safety design:
 *   - drive_control.cpp enforces a 400ms watchdog independent of the Pi.
 *   - actuator_control.cpp enforces stall-current and max-travel-time cutoffs.
 *   - The physical E-STOP should be wired directly into the motor driver's
 *     enable line in hardware. The ESTOP_PIN here is for STATUS REPORTING
 *     only — it must not be the only thing that stops the motors.
 */

#include "config.h"
#include "drive_control.h"
#include "actuator_control.h"
#include "npk_sensor.h"

volatile bool estopTriggered = false;

void estopISR() {
  estopTriggered = true;
  driveStopImmediate();
}

void setup() {
  PI_SERIAL.begin(PI_BAUD);
  driveInit();
  actuatorInit();
  npkInit();

  pinMode(ESTOP_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ESTOP_PIN), estopISR, FALLING);

  PI_SERIAL.println("STATUS IDLE");
}

void loop() {
  driveWatchdogTick();

  if (estopTriggered) {
    PI_SERIAL.println("FAULT ESTOP_TRIGGERED");
    estopTriggered = false; // report once per trip; motors already stopped
  }

  if (PI_SERIAL.available()) {
    String line = PI_SERIAL.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      handleCommand(line);
    }
  }
}

void handleCommand(String line) {
  if (line.startsWith("DRIVE")) {
    handleDriveCommand(line);
  } else if (line == "STOP") {
    driveStopImmediate();
    PI_SERIAL.println("ACK STOP");
  } else if (line.startsWith("GOTO")) {
    // Coarse-grained waypoint notification for status/logging purposes.
    // Actual navigation is driven by the Pi issuing a stream of DRIVE
    // commands based on GPS feedback; the Mega does not compute headings.
    PI_SERIAL.println("ACK GOTO");
    PI_SERIAL.println("STATUS MOVING");
    // In this simplified version, "ALIGNED" is reported immediately.
    // A future revision could hold here until the Pi sends STOP,
    // indicating the robot has reached the waypoint.
    PI_SERIAL.println("STATUS ALIGNED");
  } else if (line == "SAMPLE") {
    runSampleSequence();
  }
}

void handleDriveCommand(String line) {
  // Format: "DRIVE <FWD|BACK|LEFT|RIGHT> <speed 0-255>"
  int firstSpace = line.indexOf(' ');
  int secondSpace = line.indexOf(' ', firstSpace + 1);
  if (firstSpace < 0 || secondSpace < 0) return;

  String dirStr = line.substring(firstSpace + 1, secondSpace);
  int speed = line.substring(secondSpace + 1).toInt();
  speed = constrain(speed, 0, 255);

  DriveDirection dir = DRIVE_STOP;
  if (dirStr == "FWD") dir = DRIVE_FWD;
  else if (dirStr == "BACK") dir = DRIVE_BACK;
  else if (dirStr == "LEFT") dir = DRIVE_LEFT;
  else if (dirStr == "RIGHT") dir = DRIVE_RIGHT;

  driveSetCommand(dir, (uint8_t)speed);
  PI_SERIAL.println("ACK DRIVE");
}

void runSampleSequence() {
  PI_SERIAL.println("ACK SAMPLE");

  // Ensure the robot is stationary before inserting the probe.
  driveStopImmediate();

  PI_SERIAL.println("STATUS LOWERING");
  if (!actuatorLower()) {
    // actuatorLower() already emitted a FAULT line; abort the cycle safely.
    actuatorRaise();
    PI_SERIAL.println("STATUS IDLE");
    return;
  }

  PI_SERIAL.println("STATUS READING");
  NpkReading reading = npkRead();

  if (reading.valid) {
    PI_SERIAL.print("DATA N=");
    PI_SERIAL.print(reading.nitrogen, 1);
    PI_SERIAL.print(" P=");
    PI_SERIAL.print(reading.phosphorus, 1);
    PI_SERIAL.print(" K=");
    PI_SERIAL.print(reading.potassium, 1);
    PI_SERIAL.print(" MOIST=");
    PI_SERIAL.print(reading.moisture, 1);
    PI_SERIAL.print(" TEMP=");
    PI_SERIAL.print(reading.temperature, 1);
    PI_SERIAL.print(" EC=");
    PI_SERIAL.println(reading.ec, 3);
  } else {
    PI_SERIAL.println("FAULT NPK_SENSOR_NO_RESPONSE");
  }

  PI_SERIAL.println("STATUS RAISED");
  actuatorRaise();
  PI_SERIAL.println("STATUS IDLE");
}
