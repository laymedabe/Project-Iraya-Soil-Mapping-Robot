#include "drive_control.h"
#include "config.h"

static DriveDirection currentDirection = DRIVE_STOP;
static uint8_t currentSpeed = 0;
static unsigned long lastCommandMillis = 0;
static bool watchdogTripped = false;
static bool watchdogEnabled = false;  // Only active for serial (Pi) commands

void driveInit() {
  // Left motor driver pins
  pinMode(L_RPWM, OUTPUT); pinMode(L_LPWM, OUTPUT);
  pinMode(L_R_EN, OUTPUT); pinMode(L_L_EN, OUTPUT);

  // Right motor driver pins
  pinMode(R_RPWM, OUTPUT); pinMode(R_LPWM, OUTPUT);
  pinMode(R_R_EN, OUTPUT); pinMode(R_L_EN, OUTPUT);

  // Enable both BTS7960 drivers
  digitalWrite(L_R_EN, HIGH); digitalWrite(L_L_EN, HIGH);
  digitalWrite(R_R_EN, HIGH); digitalWrite(R_L_EN, HIGH);

  motorStop();
}

/*
 * BTS7960 dual-channel motor control.
 * Each side has RPWM (forward) and LPWM (reverse) — writing PWM to one
 * while holding the other at 0 sets direction. This matches the real
 * wiring from rccode.ino exactly.
 */

void motorForward(uint8_t speed) {
  analogWrite(L_LPWM, 0); analogWrite(R_LPWM, 0);
  analogWrite(L_RPWM, speed); analogWrite(R_RPWM, speed);
}

void motorBackward(uint8_t speed) {
  analogWrite(L_RPWM, 0); analogWrite(R_RPWM, 0);
  analogWrite(L_LPWM, speed); analogWrite(R_LPWM, speed);
}

void motorLeft(uint8_t speed) {
  // Pivot left: left side reverse, right side forward
  analogWrite(L_RPWM, 0); analogWrite(R_RPWM, 0);
  analogWrite(L_LPWM, speed); analogWrite(R_RPWM, speed);
}

void motorRight(uint8_t speed) {
  // Pivot right: left side forward, right side reverse
  analogWrite(L_RPWM, 0); analogWrite(L_LPWM, 0);
  analogWrite(L_RPWM, speed); analogWrite(R_LPWM, speed);
}

void motorStop() {
  analogWrite(L_RPWM, 0); analogWrite(L_LPWM, 0);
  analogWrite(R_RPWM, 0); analogWrite(R_LPWM, 0);
}

void driveSetCommand(DriveDirection dir, uint8_t speed) {
  currentDirection = dir;
  currentSpeed = speed;
  lastCommandMillis = millis();
  watchdogTripped = false;
  watchdogEnabled = true;  // Watchdog active for serial-originated commands

  switch (dir) {
    case DRIVE_FWD:   motorForward(speed); break;
    case DRIVE_BACK:  motorBackward(speed); break;
    case DRIVE_LEFT:  motorLeft(speed); break;
    case DRIVE_RIGHT: motorRight(speed); break;
    case DRIVE_STOP:
    default:          motorStop(); watchdogEnabled = false; break;
  }
}

void driveWatchdogTick() {
  if (!watchdogEnabled) return;
  if (currentDirection == DRIVE_STOP) return;
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
  watchdogEnabled = false;
  motorStop();
}
