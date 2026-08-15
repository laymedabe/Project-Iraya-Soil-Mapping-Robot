#ifndef IRAYA_ACTUATOR_CONTROL_H
#define IRAYA_ACTUATOR_CONTROL_H

#include <Arduino.h>

/*
 * Relay-based linear actuator control using a non-blocking state machine.
 * Matches the real hardware: a 2-channel active-LOW relay module drives the
 * actuator by reversing polarity. Timing-based control (no potentiometer
 * feedback) — extend for ACTUATOR_EXTEND_TIME_MS, hold for
 * ACTUATOR_HOLD_TIME_MS, retract for ACTUATOR_RETRACT_TIME_MS.
 */

enum ActuatorState { ACT_IDLE, ACT_EXTENDING, ACT_HOLDING, ACT_RETRACTING };

void actuatorInit();

// Begins the extend → hold → retract cycle. Call once to start; then call
// updateActuatorSequence() every loop() iteration to advance the state machine.
void startActuatorExtend();

// Non-blocking tick — call every loop(). Advances through the
// EXTENDING → HOLDING → RETRACTING → IDLE sequence based on millis().
// Returns true when the full cycle has completed back to IDLE.
// The onHoldCallback is called once when the HOLDING state begins
// (used to trigger GPS+NPK data capture).
void updateActuatorSequence();

// Immediately cuts power to both relay channels (safe stop).
void stopActuatorRelay();

// Returns the current actuator state for status reporting.
ActuatorState getActuatorState();

// Returns a human-readable label for the current state.
const char* getActuatorStateLabel();

// Flag indicating whether data capture has been performed during HOLD.
// Reset when a new cycle starts via startActuatorExtend().
extern bool samplingCompleted;

#endif
