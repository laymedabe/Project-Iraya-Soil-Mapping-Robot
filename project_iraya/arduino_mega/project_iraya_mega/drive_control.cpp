#include "drive_control.h"
#include "config.h"

static DriveDirection currentDirection = DRIVE_STOP;
static uint8_t currentSpeed = 0;
static unsigned long lastCommandMillis = 0;
static bool watchdogTripped = false;

void driveInit() {
  pinMode(LEFT_PWM_PIN, OUTPUT);
  pinMode(LEFT_DIR_PIN, OUTPUT);
  pinMode(RIGHT_PWM_PIN, OUTPUT);
  pinMode(RIGHT_DIR_PIN, OUTPUT);
  driveStopImmediate();
}

// Skid-steer mixing: FWD/BACK drive both sides equally; LEFT/RIGHT are
// implemented as a pivot turn (one side forward, one side reverse) rather
// than just slowing one side — this gives a tighter turning radius, which
// matters for headland turns between crop rows. If your chassis prefers
// gentler arcs, change the "off side" speed below from -speed to +speed*0.3.
static void applyMotors(int leftSpeed, int rightSpeed) {
  digitalWrite(LEFT_DIR_PIN, leftSpeed >= 0 ? HIGH : LOW);
  digitalWrite(RIGHT_DIR_PIN, rightSpeed >= 0 ? HIGH : LOW);
  analogWrite(LEFT_PWM_PIN, constrain(abs(leftSpeed), 0, 255));
  analogWrite(RIGHT_PWM_PIN, constrain(abs(rightSpeed), 0, 255));
}

static void applyDriveState(DriveDirection dir, uint8_t speed) {
  switch (dir) {
    case DRIVE_FWD:   applyMotors(speed, speed); break;
    case DRIVE_BACK:  applyMotors(-speed, -speed); break;
    case DRIVE_LEFT:  applyMotors(-speed, speed); break;  // pivot left
    case DRIVE_RIGHT: applyMotors(speed, -speed); break;  // pivot right
    case DRIVE_STOP:
    default:          applyMotors(0, 0); break;
  }
}

void driveSetCommand(DriveDirection dir, uint8_t speed) {
  currentDirection = dir;
  currentSpeed = speed;
  lastCommandMillis = millis();
  watchdogTripped = false;
  applyDriveState(currentDirection, currentSpeed);
}

void driveWatchdogTick() {
  if (currentDirection == DRIVE_STOP) return; // nothing to watch
  if (millis() - lastCommandMillis > DRIVE_WATCHDOG_TIMEOUT_MS) {
    if (!watchdogTripped) {
      driveStopImmediate();
      watchdogTripped = true;
      PI_SERIAL.println("FAULT DRIVE_WATCHDOG_TIMEOUT");
    }
  }
}

void driveStopImmediate() {
  currentDirection = DRIVE_STOP;
  currentSpeed = 0;
  applyMotors(0, 0);
}
