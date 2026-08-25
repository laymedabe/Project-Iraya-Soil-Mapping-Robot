/*
 * Project Orosa — Arduino Mega 2560 Unified Firmware
 *
 * Integrates the real working hardware code (rccode.ino) with the
 * Raspberry Pi serial command protocol and GPS manual capture (gps.ino).
 *
 * Control sources (all active simultaneously):
 *   1. IR Remote      — field use without the Pi (FWD/BACK/LEFT/RIGHT/STOP/ACTUATE)
 *   2. Serial from Pi — web dashboard drive control (DRIVE/STOP/SAMPLE/GOTO commands)
 *   3. Serial 'c'/'C' — manual GPS point capture (from gps.ino functionality)
 *
 * Hardware:
 *   - 2× BTS7960 motor drivers (skid-steer)
 *   - 2-channel power relay for linear actuator
 *   - MAX485 RS485 NPK soil sensor (Modbus RTU on Serial1)
 *   - NEO-M8 GPS module (on Serial2)
 *   - IR receiver (IRremote library)
 *
 * Serial protocol to Pi:
 *   Pi → Mega:  DRIVE FWD 200 | STOP | SAMPLE | GOTO lat lon | START_AUTO | STOP_AUTO
 *   Mega → Pi:  ACK ... | STATUS ... | DATA ... | FAULT ...
 *
 * Required libraries: IRremote, TinyGPS++
 */

#include <IRremote.hpp>
#include "config.h"
#include "drive_control.h"
#include "actuator_control.h"
#include "npk_sensor.h"

// =========================================================================
// STATE
// =========================================================================

volatile bool estopTriggered = false;

// =========================================================================
// AUTO-CYCLE STATE MACHINE
// =========================================================================
// Cycle: drive forward 10s → stop → sample (actuator + NPK/GPS) → repeat

enum AutoCycleState { AUTO_OFF, AUTO_DRIVING, AUTO_SAMPLING };
AutoCycleState autoCycleState = AUTO_OFF;
unsigned long autoDriveStart = 0;

// =========================================================================
// INTERRUPT: E-STOP
// =========================================================================

void stopAutoCycle();

void estopISR() {
  estopTriggered = true;
  autoCycleState = AUTO_OFF;  // Cancel auto mode on E-STOP
  driveStopImmediate();
}

// =========================================================================
// SETUP
// =========================================================================

void setup() {
  PI_SERIAL.begin(PI_BAUD);     // USB to Pi / Serial Monitor (9600)

  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  driveInit();
  actuatorInit();
  npkInit();

  pinMode(ESTOP_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ESTOP_PIN), estopISR, FALLING);

  PI_SERIAL.println(F("=========================================================="));
  PI_SERIAL.println(F("  DUTA: Digitalized Unified Terrain Assessment            "));
  PI_SERIAL.println(F("    Unified Firmware (IR + Serial)                         "));
  PI_SERIAL.println(F("=========================================================="));
  PI_SERIAL.println(F("Controls:"));
  PI_SERIAL.println(F("  IR Remote : FWD/BACK/LEFT/RIGHT/STOP/ACTUATE buttons"));
  PI_SERIAL.println(F("  Serial    : DRIVE FWD 200 | STOP | SAMPLE | START_AUTO | STOP_AUTO"));
  PI_SERIAL.println(F("----------------------------------------------------------"));
  PI_SERIAL.println("STATUS IDLE");
}

// =========================================================================
// MAIN LOOP
// =========================================================================

void loop() {
  // 1. Drive watchdog (only for serial-originated manual commands)
  if (autoCycleState == AUTO_OFF) {
    driveWatchdogTick();
  }

  // 2. Auto-cycle state machine tick
  autoCycleTick();

  // 3. E-STOP reporting
  if (estopTriggered) {
    stopAutoCycle();
    PI_SERIAL.println("FAULT ESTOP_TRIGGERED");
    estopTriggered = false;
  }

  // 4. Decode and execute incoming IR Remote commands
  if (IrReceiver.decode()) {
    unsigned long receivedCode = IrReceiver.decodedIRData.decodedRawData;

    if (receivedCode != 0 && !(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
      handleIRCommand(receivedCode);
    }

    IrReceiver.resume();
  }

  // 5. Process serial commands from the Pi or Serial Monitor
  if (PI_SERIAL.available()) {
    String line = PI_SERIAL.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      handleSerialCommand(line);
    }
  }

  // 6. Process non-blocking actuator state machine
  updateActuatorSequence();

  // 7. If auto-cycle is sampling and actuator just returned to IDLE,
  //    transition back to driving for the next leg
  if (autoCycleState == AUTO_SAMPLING && getActuatorState() == ACT_IDLE) {
    PI_SERIAL.println(F(">> Auto-cycle: sample complete, resuming drive..."));
    PI_SERIAL.println("STATUS AUTO_DRIVING");
    motorForward(MOTOR_SPEED);
    autoDriveStart = millis();
    autoCycleState = AUTO_DRIVING;
  }
}

// =========================================================================
// IR REMOTE COMMAND HANDLER
// =========================================================================

void handleIRCommand(unsigned long code) {
  if (code == IR_FORWARD) {
    motorForward(MOTOR_SPEED);
  } else if (code == IR_BACKWARD) {
    motorBackward(MOTOR_SPEED);
  } else if (code == IR_LEFT) {
    motorLeft(MOTOR_SPEED);
  } else if (code == IR_RIGHT) {
    motorRight(MOTOR_SPEED);
  } else if (code == IR_STOP) {
    stopAutoCycle();  // Cancel auto mode if active
    motorStop();
    stopActuatorRelay();
  } else if (code == IR_ACTUATE && getActuatorState() == ACT_IDLE) {
    motorStop();  // Lock wheels during sampling
    PI_SERIAL.println(F("\n>> COMMAND RECEIVED: Lowering probe..."));
    PI_SERIAL.println("ACK SAMPLE");
    PI_SERIAL.println("STATUS LOWERING");
    startActuatorExtend();
  }
}

// =========================================================================
// SERIAL COMMAND HANDLER (from Raspberry Pi)
// =========================================================================

void handleSerialCommand(String line) {
  if (line.startsWith("DRIVE")) {
    handleDriveCommand(line);
  } else if (line == "STOP") {
    stopAutoCycle();  // Cancel auto mode if active
    driveStopImmediate();
    stopActuatorRelay();
    PI_SERIAL.println("ACK STOP");
  } else if (line == "START_AUTO") {
    if (autoCycleState != AUTO_OFF) {
      PI_SERIAL.println("FAULT AUTO_ALREADY_RUNNING");
      return;
    }
    PI_SERIAL.println("ACK START_AUTO");
    PI_SERIAL.println(F(">> Auto-cycle started: drive 10s → sample → repeat"));
    PI_SERIAL.println("STATUS AUTO_DRIVING");
    motorForward(MOTOR_SPEED);
    autoDriveStart = millis();
    autoCycleState = AUTO_DRIVING;
  } else if (line == "STOP_AUTO") {
    stopAutoCycle();
    PI_SERIAL.println("ACK STOP_AUTO");
  } else if (line.startsWith("GOTO")) {
    // Coarse-grained waypoint notification for status/logging purposes.
    // Actual navigation is driven by the Pi issuing a stream of DRIVE
    // commands based on GPS feedback; the Mega does not compute headings.
    PI_SERIAL.println("ACK GOTO");
    PI_SERIAL.println("STATUS MOVING");
    PI_SERIAL.println("STATUS ALIGNED");
  } else if (line == "SAMPLE") {
    if (getActuatorState() != ACT_IDLE) {
      PI_SERIAL.println("FAULT ACTUATOR_BUSY");
      return;
    }
    driveStopImmediate();  // Lock wheels during sampling
    PI_SERIAL.println("ACK SAMPLE");
    PI_SERIAL.println("STATUS LOWERING");
    startActuatorExtend();
  }
}

void handleDriveCommand(String line) {
  // Format: "DRIVE <FWD|BACK|LEFT|RIGHT|STOP> <speed 0-255>"
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

// =========================================================================
// AUTO-CYCLE HELPERS
// =========================================================================

void autoCycleTick() {
  if (autoCycleState == AUTO_DRIVING) {
    // Check if the 10-second drive interval has elapsed
    if (millis() - autoDriveStart >= AUTO_DRIVE_INTERVAL_MS) {
      PI_SERIAL.println(F(">> Auto-cycle: 10s drive complete, stopping to sample..."));
      motorStop();
      PI_SERIAL.println("STATUS AUTO_SAMPLING");
      PI_SERIAL.println("STATUS LOWERING");
      startActuatorExtend();
      autoCycleState = AUTO_SAMPLING;
    }
  }
  // AUTO_SAMPLING → AUTO_DRIVING transition is handled in the main loop
  // (after actuator returns to IDLE)
}

void stopAutoCycle() {
  if (autoCycleState != AUTO_OFF) {
    autoCycleState = AUTO_OFF;
    motorStop();
    stopActuatorRelay();
    PI_SERIAL.println(F(">> Auto-cycle stopped."));
    PI_SERIAL.println("STATUS IDLE");
  }
}
