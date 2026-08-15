#ifndef IRAYA_DRIVE_CONTROL_H
#define IRAYA_DRIVE_CONTROL_H

#include <Arduino.h>

enum DriveDirection { DRIVE_STOP, DRIVE_FWD, DRIVE_BACK, DRIVE_LEFT, DRIVE_RIGHT };

void driveInit();

// Sets the requested drive state and resets the watchdog timer.
// Called every time a fresh "DRIVE ..." command line arrives from the Pi
// or an IR remote button is pressed.
void driveSetCommand(DriveDirection dir, uint8_t speed);

// Must be called every loop() iteration. Enforces the watchdog: if no
// fresh command has arrived within DRIVE_WATCHDOG_TIMEOUT_MS, forces a stop
// regardless of the last commanded state. This is the primary defense
// against a dropped Wi-Fi link or a hung Raspberry Pi process.
// NOTE: The watchdog only applies to serial (Pi) commands. IR remote
// commands are one-shot (press once → drive until STOP), so the watchdog
// is bypassed for IR input.
void driveWatchdogTick();

// Immediately zeroes both motor channels. Safe to call at any time,
// including from the E-STOP interrupt handler.
void driveStopImmediate();

// Direct motor control functions matching rccode.ino BTS7960 wiring.
// These are also called by driveSetCommand internally.
void motorForward(uint8_t speed);
void motorBackward(uint8_t speed);
void motorLeft(uint8_t speed);
void motorRight(uint8_t speed);
void motorStop();

#endif
