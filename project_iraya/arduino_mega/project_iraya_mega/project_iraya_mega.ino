/*
 * Project Iraya — Arduino Mega 2560 Unified Firmware
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
 *   - NEO-6M GPS module (on Serial2)
 *   - IR receiver (IRremote library)
 *
 * Serial protocol to Pi:
 *   Pi → Mega:  DRIVE FWD 200 | STOP | SAMPLE | GOTO lat lon
 *   Mega → Pi:  ACK ... | STATUS ... | DATA ... | FAULT ...
 *
 * Required libraries: IRremote, TinyGPS++
 */

#include <IRremote.hpp>
#include <TinyGPS++.h>
#include "config.h"
#include "drive_control.h"
#include "actuator_control.h"
#include "npk_sensor.h"

// =========================================================================
// GPS & STATE
// =========================================================================

TinyGPSPlus gps;
int pointCount = 0;  // Incremental ID for logged soil/GPS points

volatile bool estopTriggered = false;

// =========================================================================
// INTERRUPT: E-STOP
// =========================================================================

void estopISR() {
  estopTriggered = true;
  driveStopImmediate();
}

// =========================================================================
// SETUP
// =========================================================================

void setup() {
  PI_SERIAL.begin(PI_BAUD);     // USB to Pi / Serial Monitor (9600)
  RS485_SERIAL.begin(NPK_BAUD); // NPK Sensor via MAX485 (9600)
  GPS_SERIAL.begin(GPS_BAUD);   // NEO-6M GPS module (9600)

  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  driveInit();
  actuatorInit();
  npkInit();

  pinMode(ESTOP_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ESTOP_PIN), estopISR, FALLING);

  PI_SERIAL.println(F("=========================================================="));
  PI_SERIAL.println(F("    PROJECT IRAYA — SMART NPK SOIL MAPPING ROBOT          "));
  PI_SERIAL.println(F("    Unified Firmware (IR + Serial + GPS Capture)           "));
  PI_SERIAL.println(F("=========================================================="));
  PI_SERIAL.println(F("Controls:"));
  PI_SERIAL.println(F("  IR Remote : FWD/BACK/LEFT/RIGHT/STOP/ACTUATE buttons"));
  PI_SERIAL.println(F("  Serial    : DRIVE FWD 200 | STOP | SAMPLE | GOTO lat lon"));
  PI_SERIAL.println(F("  Type 'c'  : Capture current GPS position (manual point)"));
  PI_SERIAL.println(F("----------------------------------------------------------"));
  PI_SERIAL.println(F("CSV Format: Point_ID, Lat, Lon, Alt_m, Sats, HDOP, NPK_Data"));
  PI_SERIAL.println(F("----------------------------------------------------------"));
  PI_SERIAL.println("STATUS IDLE");
}

// =========================================================================
// MAIN LOOP
// =========================================================================

void loop() {
  // 1. Continuously feed raw GPS stream to TinyGPS++ parser
  while (GPS_SERIAL.available() > 0) {
    gps.encode(GPS_SERIAL.read());
  }

  // 2. Drive watchdog (only for serial-originated commands)
  driveWatchdogTick();

  // 3. E-STOP reporting
  if (estopTriggered) {
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
      // Check for single-char GPS capture command (from gps.ino)
      if (line.length() == 1 && (line.charAt(0) == 'c' || line.charAt(0) == 'C')) {
        captureGPSPosition();
      } else {
        handleSerialCommand(line);
      }
    }
  }

  // 6. Process non-blocking actuator state machine
  //    During HOLDING state, perform data capture if not yet done
  if (getActuatorState() == ACT_HOLDING && !samplingCompleted) {
    captureSoilAndGPSData();
    samplingCompleted = true;
  }
  updateActuatorSequence();
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
    motorStop();
    stopActuatorRelay();
    // If actuator was mid-cycle, force it back to idle
  } else if (code == IR_ACTUATE && getActuatorState() == ACT_IDLE) {
    motorStop();  // Lock wheels during sampling
    PI_SERIAL.println(F("\n>> COMMAND RECEIVED: Lowering probe and capturing soil coordinates..."));
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
    driveStopImmediate();
    stopActuatorRelay();
    PI_SERIAL.println("ACK STOP");
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
// DATA CAPTURE: GPS + NPK SOIL READING
// =========================================================================

void captureSoilAndGPSData() {
  pointCount++;

  PI_SERIAL.println(F("\n--- [ DATA LOG MAP ROW ] ---"));

  // Emit structured DATA line for the Pi's serial_comm.py parser
  PI_SERIAL.print(F("DATA POINT="));
  PI_SERIAL.print(pointCount);

  // GPS coordinates
  if (gps.location.isValid()) {
    PI_SERIAL.print(F(" LAT="));
    PI_SERIAL.print(gps.location.lat(), 6);
    PI_SERIAL.print(F(" LON="));
    PI_SERIAL.print(gps.location.lng(), 6);
    PI_SERIAL.print(F(" ALT="));
    PI_SERIAL.print(gps.altitude.meters(), 2);
    PI_SERIAL.print(F(" SAT="));
    PI_SERIAL.print(gps.satellites.value());
    PI_SERIAL.print(F(" HDOP="));
    if (gps.hdop.isValid()) {
      PI_SERIAL.print(gps.hdop.hdop(), 2);
    } else {
      PI_SERIAL.print(F("0"));
    }
  } else {
    PI_SERIAL.print(F(" LAT=0 LON=0 ALT=0 SAT=0 HDOP=0 NOFIX=1"));
  }

  // Query NPK sensor
  NpkReading npk = npkRead();

  if (npk.valid) {
    PI_SERIAL.print(F(" N="));
    PI_SERIAL.print(npk.nitrogen, 1);
    PI_SERIAL.print(F(" P="));
    PI_SERIAL.print(npk.phosphorus, 1);
  } else {
    PI_SERIAL.print(F(" N=0 P=0 NPKERR=1"));
  }

  PI_SERIAL.println();

  // Also emit the CSV-style log line for human-readable Serial Monitor
  // (matches the format from rccode.ino)
  PI_SERIAL.print(F("CSV_LOG: "));
  PI_SERIAL.print(pointCount);
  PI_SERIAL.print(F(","));

  if (gps.location.isValid()) {
    PI_SERIAL.print(gps.location.lat(), 6);
    PI_SERIAL.print(F(","));
    PI_SERIAL.print(gps.location.lng(), 6);
    PI_SERIAL.print(F(","));
    PI_SERIAL.print(gps.altitude.meters(), 2);
    PI_SERIAL.print(F(","));
    PI_SERIAL.print(gps.satellites.value());
    PI_SERIAL.print(F(","));
    if (gps.hdop.isValid()) {
      PI_SERIAL.print(gps.hdop.hdop(), 2);
    } else {
      PI_SERIAL.print(F("N/A"));
    }
  } else {
    PI_SERIAL.print(F("NO_GPS_FIX,NO_GPS_FIX,0.0,0,N/A"));
  }

  PI_SERIAL.print(F(",NPK_DATA: "));
  npkPrintRawHex(npk);
  PI_SERIAL.println();
}

// =========================================================================
// MANUAL GPS POSITION CAPTURE (merged from gps.ino)
// =========================================================================

void captureGPSPosition() {
  if (gps.location.isValid()) {
    pointCount++;

    // Structured line for Pi
    PI_SERIAL.print(F("DATA POINT="));
    PI_SERIAL.print(pointCount);
    PI_SERIAL.print(F(" LAT="));
    PI_SERIAL.print(gps.location.lat(), 6);
    PI_SERIAL.print(F(" LON="));
    PI_SERIAL.print(gps.location.lng(), 6);
    PI_SERIAL.print(F(" ALT="));
    PI_SERIAL.print(gps.altitude.meters(), 2);
    PI_SERIAL.print(F(" SAT="));
    PI_SERIAL.print(gps.satellites.value());
    PI_SERIAL.print(F(" HDOP="));
    if (gps.hdop.isValid()) {
      PI_SERIAL.print(gps.hdop.hdop(), 2);
    } else {
      PI_SERIAL.print(F("0"));
    }
    PI_SERIAL.print(F(" N=0 P=0 GPSONLY=1"));
    PI_SERIAL.println();

    // Human-readable CSV line
    PI_SERIAL.print(F("GPS_LOG: "));
    PI_SERIAL.print(pointCount);
    PI_SERIAL.print(F(","));
    PI_SERIAL.print(gps.location.lat(), 6);
    PI_SERIAL.print(F(","));
    PI_SERIAL.print(gps.location.lng(), 6);
    PI_SERIAL.print(F(","));
    PI_SERIAL.print(gps.altitude.meters(), 2);
    PI_SERIAL.print(F(","));
    PI_SERIAL.print(gps.satellites.value());
    PI_SERIAL.print(F(","));
    if (gps.hdop.isValid()) {
      PI_SERIAL.println(gps.hdop.hdop(), 2);
    } else {
      PI_SERIAL.println(F("N/A"));
    }
  } else {
    PI_SERIAL.println(F("[ERROR] Cannot log point: No valid 3D GPS fix acquired yet."));
  }
}
