#include <IRremote.hpp>
#include <SoftwareSerial.h>

// =========================================================================
// PIN DEFINITIONS & SYSTEM CONFIGURATION (Arduino Uno)
// =========================================================================

// IR Receiver Pin
const int IR_RECEIVE_PIN = 2;

// Left Motor Driver (BTS7960 #1) - Timer 0 PWM
const int L_RPWM = 5; 
const int L_LPWM = 6; 
const int L_R_EN = 7; 
const int L_L_EN = 8; 

// Right Motor Driver (BTS7960 #2) - Timer 1 PWM (Avoids Timer 2 conflicts with IRremote)
const int R_RPWM = 9; 
const int R_LPWM = 10; 
const int R_R_EN = 4; 
const int R_L_EN = 12; 

// Linear Actuator 2-Channel Power Relay Module
const int RELAY_ACT_1 = A0; // Controls Actuator Lead 1
const int RELAY_ACT_2 = A1; // Controls Actuator Lead 2

// MAX485 TTL RS485 Module Pins for 7-in-1 Sensor
const int RS485_RX = A2; // Arduino RX <- MAX485 RO
const int RS485_TX = A3; // Arduino TX -> MAX485 DI
const int RS485_DE_RE = A4; // RS485 Flow Control (DE and RE tied together)

SoftwareSerial npkSerial(RS485_RX, RS485_TX);

// Modbus RTU Command: Requesting 7 registers starting at address 0x0000
const byte allParamsRequestFrame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08};
byte allParamsResponseFrame[19]; // 3 header bytes + 14 data bytes + 2 CRC bytes = 19 bytes

// =========================================================================
// SPEED & TIMING PARAMETERS
// =========================================================================

const int MOTOR_SPEED        = 200;   // Drivetrain PWM Speed (0 to 255)
const unsigned long EXTEND_TIME  = 7000;  // Actuator extend duration (ms)
const unsigned long HOLD_TIME    = 10000; // Time probe stays lowered (ms)
const unsigned long RETRACT_TIME = 7000;  // Actuator retract duration (ms)
const unsigned long SENSOR_INTERVAL = 1000; // Read sensor every 1 second during hold

// =========================================================================
// IR REMOTE HEX COMMANDS
// =========================================================================

const unsigned long IR_FORWARD  = 0xBC43FF00; 
const unsigned long IR_BACKWARD = 0xBB44FF00; 
const unsigned long IR_LEFT     = 0xB946FF00; 
const unsigned long IR_RIGHT    = 0xEA15FF00; 
const unsigned long IR_STOP     = 0xAD52FF00; 
const unsigned long IR_ACTUATE  = 0xBF40FF00; // Triggers Linear Actuator Sequence

// =========================================================================
// STATE VARIABLES
// =========================================================================

enum ActuatorState { ACT_IDLE, ACT_EXTENDING, ACT_HOLDING, ACT_RETRACTING };
ActuatorState actState = ACT_IDLE;
unsigned long actTimer = 0;
unsigned long lastSensorReadTimer = 0; // Tracks 1-second intervals during holding

// Function Declarations
void motorForward();
void motorBackward();
void motorLeft();
void motorRight();
void motorStop();
void startActuatorExtend();
void updateActuatorSequence();
void stopActuatorRelay();
void readNPKSensor();

// =========================================================================
// INITIAL SETUP
// =========================================================================

void setup() {
  Serial.begin(9600);
  npkSerial.begin(4800); // Configured for 4800 baud

  // MAX485 Control Pin Setup
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW); // Receive mode by default

  // Safely set relays off BEFORE declaring outputs to prevent startup relay clicks
  digitalWrite(RELAY_ACT_1, HIGH);
  digitalWrite(RELAY_ACT_2, HIGH);
  pinMode(RELAY_ACT_1, OUTPUT);
  pinMode(RELAY_ACT_2, OUTPUT);

  // Motor Driver Pin Modes
  pinMode(L_RPWM, OUTPUT); pinMode(L_LPWM, OUTPUT);
  pinMode(L_R_EN, OUTPUT); pinMode(L_L_EN, OUTPUT);
  pinMode(R_RPWM, OUTPUT); pinMode(R_LPWM, OUTPUT);
  pinMode(R_R_EN, OUTPUT); pinMode(R_L_EN, OUTPUT);

  // Enable BTS7960 Drivers
  digitalWrite(L_R_EN, HIGH); digitalWrite(L_L_EN, HIGH);
  digitalWrite(R_R_EN, HIGH); digitalWrite(R_L_EN, HIGH);

  // Start IR Receiver
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  motorStop();

  Serial.println(F("=========================================================="));
  Serial.println(F("     ARDUINO UNO ROBOT ACTUATOR SYSTEM INITIALIZED        "));
  Serial.println(F("=========================================================="));
}

// =========================================================================
// MAIN CONTROL LOOP
// =========================================================================

void loop() {
  // Check IR remote ONLY if the actuator cycle is NOT active (24-second lockout)
  if (actState == ACT_IDLE) {
    if (IrReceiver.decode()) {
      unsigned long receivedCode = IrReceiver.decodedIRData.decodedRawData;

      if (receivedCode != 0 && !(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
        if (receivedCode == IR_FORWARD)       { motorForward(); } 
        else if (receivedCode == IR_BACKWARD) { motorBackward(); } 
        else if (receivedCode == IR_LEFT)     { motorLeft(); }
        else if (receivedCode == IR_RIGHT)    { motorRight(); }
        else if (receivedCode == IR_STOP)     { motorStop(); stopActuatorRelay(); }
        else if (receivedCode == IR_ACTUATE)  {
          motorStop(); // Lock wheels during the 24s sequence
          Serial.println(F("\n>> ACTUATOR STARTED: All remote buttons locked for 24s..."));
          startActuatorExtend();
        }
      }

      IrReceiver.resume(); 
    }
  } else {
    // Clear out any buffered IR signals received during the 24s lockout period
    if (IrReceiver.decode()) {
      IrReceiver.resume();
    }
  }

  // Non-blocking state machine for linear actuator
  updateActuatorSequence();
}

// =========================================================================
// ACTUATOR AUTOMATION & STATE MACHINE
// =========================================================================

void startActuatorExtend() {
  actState = ACT_EXTENDING;
  actTimer = millis();
  
  // Extend Actuator (Active-LOW Relays)
  digitalWrite(RELAY_ACT_1, LOW);  // Lead 1 -> +12V
  digitalWrite(RELAY_ACT_2, HIGH); // Lead 2 -> GND
}

void updateActuatorSequence() {
  switch (actState) {
    
    case ACT_EXTENDING:
      if (millis() - actTimer >= EXTEND_TIME) {
        Serial.println(F(">> Probe fully lowered (7s elapsed). Starting 10s continuous monitoring..."));
        stopActuatorRelay();
        actState = ACT_HOLDING;
        actTimer = millis();
        lastSensorReadTimer = 0; // Trigger immediate first reading
      }
      break;

    case ACT_HOLDING:
      // Read sensor continuously every 1 second throughout the 10-second hold period
      if (millis() - lastSensorReadTimer >= SENSOR_INTERVAL) {
        lastSensorReadTimer = millis();
        readNPKSensor();
      }

      if (millis() - actTimer >= HOLD_TIME) {
        Serial.println(F(">> Hold duration complete (10s elapsed). Retracting for 7s..."));
        actState = ACT_RETRACTING;
        actTimer = millis();
        
        // Retract Actuator (Active-LOW Relays)
        digitalWrite(RELAY_ACT_1, HIGH); // Lead 1 -> GND
        digitalWrite(RELAY_ACT_2, LOW);  // Lead 2 -> +12V
      }
      break;

    case ACT_RETRACTING:
      if (millis() - actTimer >= RETRACT_TIME) {
        Serial.println(F(">> Probe fully retracted (7s elapsed). Sensor set to IDLE. Sequence complete."));
        Serial.println(F(">> Remote control unlocked. Ready to navigate."));
        Serial.println(F("----------------------------------------------------------\n"));
        stopActuatorRelay();
        actState = ACT_IDLE; // Unlocks remote buttons
      }
      break;

    case ACT_IDLE:
    default:
      break;
  }
}

void stopActuatorRelay() {
  // Active-LOW Relays turn OFF when driven HIGH
  digitalWrite(RELAY_ACT_1, HIGH);
  digitalWrite(RELAY_ACT_2, HIGH);
}

// =========================================================================
// 7-IN-1 SENSOR RS485 READ ROUTINE
// =========================================================================

void readNPKSensor() {
  // Clear incoming serial buffer
  while (npkSerial.available()) { npkSerial.read(); }

  // Switch MAX485 to Transmit mode
  digitalWrite(RS485_DE_RE, HIGH);
  delay(10);

  // Send Modbus Request Frame
  npkSerial.write(allParamsRequestFrame, sizeof(allParamsRequestFrame));
  npkSerial.flush();

  // Switch MAX485 back to Receive mode
  digitalWrite(RS485_DE_RE, LOW);
  delay(10);

  // Wait for Modbus Response
  unsigned long timeout = millis();
  byte index = 0;

  while ((millis() - timeout < 500) && (index < sizeof(allParamsResponseFrame))) {
    if (npkSerial.available()) {
      allParamsResponseFrame[index++] = npkSerial.read();
    }
  }

  // Process data if valid frame received (19 bytes expected)
  if (index >= 19) {
    float moisture    = ((allParamsResponseFrame[3] << 8)  | allParamsResponseFrame[4]) / 10.0;
    float temperature = ((allParamsResponseFrame[5] << 8)  | allParamsResponseFrame[6]) / 10.0;
    uint16_t ec       =  (allParamsResponseFrame[7] << 8)  | allParamsResponseFrame[8];
    float ph          = ((allParamsResponseFrame[9] << 8)  | allParamsResponseFrame[10]) / 10.0;
    uint16_t nitrogen =  (allParamsResponseFrame[11] << 8) | allParamsResponseFrame[12];
    uint16_t phos     =  (allParamsResponseFrame[13] << 8) | allParamsResponseFrame[14];
    uint16_t potass   =  (allParamsResponseFrame[15] << 8) | allParamsResponseFrame[16];

    Serial.println(F("--- 7-IN-1 SOIL SENSOR READINGS ---"));
    Serial.print(F("Moisture    : ")); Serial.print(moisture); Serial.println(F(" %"));
    Serial.print(F("Temperature : ")); Serial.print(temperature); Serial.println(F(" deg C"));
    Serial.print(F("EC          : ")); Serial.print(ec); Serial.println(F(" us/cm"));
    Serial.print(F("pH Level    : ")); Serial.println(ph);
    Serial.print(F("Nitrogen (N): ")); Serial.print(nitrogen); Serial.println(F(" mg/kg"));
    Serial.print(F("Phosphorus  : ")); Serial.print(phos); Serial.println(F(" mg/kg"));
    Serial.print(F("Potassium   : ")); Serial.print(potass); Serial.println(F(" mg/kg"));
    Serial.println(F("-----------------------------------"));
  } else {
    Serial.println(F(">> [ERROR] 7-in-1 Sensor Response Timeout or Bad Connection."));
  }
}

// =========================================================================
// DRIVETRAIN MOTOR CONTROL ROUTINES (BTS7960)
// =========================================================================

void motorForward() {
  analogWrite(L_RPWM, 0); 
  analogWrite(R_RPWM, 0);
  analogWrite(L_LPWM, MOTOR_SPEED); 
  analogWrite(R_LPWM, MOTOR_SPEED);
}

void motorBackward() {
  analogWrite(L_LPWM, 0); 
  analogWrite(R_LPWM, 0);
  analogWrite(L_RPWM, MOTOR_SPEED); 
  analogWrite(R_RPWM, MOTOR_SPEED);
}

void motorLeft() {
  analogWrite(L_LPWM, 0); 
  analogWrite(R_RPWM, 0);
  analogWrite(L_RPWM, MOTOR_SPEED); 
  analogWrite(R_LPWM, MOTOR_SPEED);
}

void motorRight() {
  analogWrite(R_LPWM, 0); 
  analogWrite(L_RPWM, 0);
  analogWrite(L_LPWM, MOTOR_SPEED); 
  analogWrite(R_RPWM, MOTOR_SPEED);
}

void motorStop() {
  analogWrite(L_RPWM, 0); 
  analogWrite(L_LPWM, 0);
  analogWrite(R_RPWM, 0); 
  analogWrite(R_LPWM, 0);
}