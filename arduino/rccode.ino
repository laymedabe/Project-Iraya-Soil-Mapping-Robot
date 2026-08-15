#include <IRremote.hpp>
#include <TinyGPS++.h>

// =========================================================================
// PIN DEFINITIONS & SYSTEM CONFIGURATION (Arduino Mega 2560)
// =========================================================================

// IR Receiver Pin
const int IR_RECEIVE_PIN = 11;

// Left Motor Driver (BTS7960 #1)
const int L_RPWM = 5; 
const int L_LPWM = 6; 
const int L_R_EN = 7; 
const int L_L_EN = 8; 

// Right Motor Driver (BTS7960 #2)
// (Moved to Pins 44 & 45 to avoid Timer 2 PWM conflicts with IRremote)
const int R_RPWM = 44; 
const int R_LPWM = 45; 
const int R_R_EN = 4; 
const int R_L_EN = 2; 

// Linear Actuator 2-Channel Power Relay Module
const int RELAY_ACT_1 = 22; // Controls Actuator Lead 1
const int RELAY_ACT_2 = 23; // Controls Actuator Lead 2

// MAX485 TTL to RS485 Control Pin
const int DE_RE_PIN = 24; 

// Hardware Serial Allocations:
// - Serial  (Pins 0/1):   USB PC Serial Monitor (9600 baud)
// - Serial1 (Pins 18/19): RS485 NPK Soil Sensor (9600 baud)
// - Serial2 (Pins 16/17): NEO-6M GPS Module (9600 baud)

// =========================================================================
// SPEED & TIMING PARAMETERS
// =========================================================================

const int MOTOR_SPEED            = 200;  // Drivetrain PWM Speed (0 to 255)
const unsigned long EXTEND_TIME  = 3000; // Actuator extend duration (ms)
const unsigned long HOLD_TIME    = 5000; // Time probe stays in soil (ms)
const unsigned long RETRACT_TIME = 3000; // Actuator retract duration (ms)

// =========================================================================
// IR REMOTE HEX COMMANDS
// =========================================================================

const unsigned long IR_FORWARD  = 0xB946FF00; 
const unsigned long IR_BACKWARD = 0xEA15FF00; 
const unsigned long IR_LEFT     = 0xBB44FF00; 
const unsigned long IR_RIGHT    = 0xBC43FF00; 
const unsigned long IR_STOP     = 0xBF40FF00; 
const unsigned long IR_ACTUATE  = 0xF708FF00; // Triggers Actuator + GPS + NPK

// =========================================================================
// SENSOR PROTOCOL & STATE VARIABLES
// =========================================================================

// Modbus RTU Command to Read Holding Registers (NPK Sensor)
const byte sensorRequest[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B}; 

TinyGPSPlus gps;
int pointCount = 0; // Incremental ID for logged soil points

enum ActuatorState { ACT_IDLE, ACT_EXTENDING, ACT_HOLDING, ACT_RETRACTING };
ActuatorState actState = ACT_IDLE;
unsigned long actTimer = 0;
bool samplingCompleted = false;

// Function Declarations
void motorForward();
void motorBackward();
void motorLeft();
void motorRight();
void motorStop();
void startActuatorExtend();
void updateActuatorSequence();
void stopActuatorRelay();
void captureSoilAndGPSData();
void readNPKSensor();
void printHexArray(const byte* array, size_t length);

// =========================================================================
// INITIAL SETUP
// =========================================================================

void setup() {
  Serial.begin(9600);    // PC Serial Monitor Output
  Serial1.begin(9600);   // NPK Sensor (MAX485 on Pins 18/19)
  Serial2.begin(9600);   // GPS Module (Pins 16/17)

  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  // Motor Driver Pin Modes
  pinMode(L_RPWM, OUTPUT); pinMode(L_LPWM, OUTPUT);
  pinMode(L_R_EN, OUTPUT); pinMode(L_L_EN, OUTPUT);
  pinMode(R_RPWM, OUTPUT); pinMode(R_LPWM, OUTPUT);
  pinMode(R_R_EN, OUTPUT); pinMode(R_L_EN, OUTPUT);

  // Enable BTS7960 Drivers
  digitalWrite(L_R_EN, HIGH); digitalWrite(L_L_EN, HIGH);
  digitalWrite(R_R_EN, HIGH); digitalWrite(R_L_EN, HIGH);

  // Relay Pin Modes
  pinMode(RELAY_ACT_1, OUTPUT);
  pinMode(RELAY_ACT_2, OUTPUT);

  // MAX485 Control Pin Mode
  pinMode(DE_RE_PIN, OUTPUT);
  digitalWrite(DE_RE_PIN, LOW); // Receiver Mode by default

  motorStop();
  stopActuatorRelay();

  Serial.println(F("=========================================================="));
  Serial.println(F("    SMART NPK SOIL MAPPING ROBOT SYSTEM INITIALIZED       "));
  Serial.println(F("=========================================================="));
  Serial.println(F("CSV Format: Point_ID, Latitude, Longitude, Alt_m, Sats, HDOP, NPK_Raw_Hex"));
  Serial.println(F("----------------------------------------------------------"));
}

// =========================================================================
// MAIN CONTROL LOOP
// =========================================================================

void loop() {
  // 1. Constantly feed raw GPS stream to TinyGPS++ parser
  while (Serial2.available() > 0) {
    gps.encode(Serial2.read());
  }

  // 2. Decode and execute incoming IR Remote commands
  if (IrReceiver.decode()) {
    unsigned long receivedCode = IrReceiver.decodedIRData.decodedRawData;

    if (receivedCode != 0 && !(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
      if (receivedCode == IR_FORWARD)       { motorForward(); } 
      else if (receivedCode == IR_BACKWARD) { motorBackward(); } 
      else if (receivedCode == IR_LEFT)     { motorLeft(); }
      else if (receivedCode == IR_RIGHT)    { motorRight(); }
      else if (receivedCode == IR_STOP)     { motorStop(); stopActuatorRelay(); actState = ACT_IDLE; }
      else if (receivedCode == IR_ACTUATE && actState == ACT_IDLE) {
        motorStop(); // Lock wheels during sampling
        Serial.println(F("\n>> COMMAND RECEIVED: Lowering probe and capturing soil coordinates..."));
        startActuatorExtend();
      }
    }

    IrReceiver.resume(); 
  }

  // 3. Process non-blocking state machine for linear actuator and soil test
  updateActuatorSequence();
}

// =========================================================================
// ACTUATOR AUTOMATION & STATE MACHINE
// =========================================================================

void startActuatorExtend() {
  actState = ACT_EXTENDING;
  actTimer = millis();
  samplingCompleted = false;
  
  // Extend Actuator (Active-LOW Relays)
  digitalWrite(RELAY_ACT_1, LOW);  // Lead 1 -> +12V
  digitalWrite(RELAY_ACT_2, HIGH); // Lead 2 -> GND
}

void updateActuatorSequence() {
  switch (actState) {
    
    case ACT_EXTENDING:
      if (millis() - actTimer >= EXTEND_TIME) {
        Serial.println(F(">> Probe inserted into soil. Reading NPK & GPS mapping data..."));
        stopActuatorRelay();
        actState = ACT_HOLDING;
        actTimer = millis();
      }
      break;

    case ACT_HOLDING:
      // Perform single synchronized GPS + NPK capture during soil insertion
      if (!samplingCompleted) {
        captureSoilAndGPSData();
        samplingCompleted = true;
      }

      // Hold probe extended in soil for 5 seconds total
      if (millis() - actTimer >= HOLD_TIME) {
        Serial.println(F(">> Sampling complete. Retracting probe..."));
        actState = ACT_RETRACTING;
        actTimer = millis();
        
        // Retract Actuator (Active-LOW Relays)
        digitalWrite(RELAY_ACT_1, HIGH); // Lead 1 -> GND
        digitalWrite(RELAY_ACT_2, LOW);  // Lead 2 -> +12V
      }
      break;

    case ACT_RETRACTING:
      if (millis() - actTimer >= RETRACT_TIME) {
        Serial.println(F(">> Probe fully retracted. Robot ready to navigate."));
        Serial.println(F("----------------------------------------------------------\n"));
        stopActuatorRelay();
        actState = ACT_IDLE;
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
// DATA LOGGING & SENSOR ROUTINES
// =========================================================================

void captureSoilAndGPSData() {
  pointCount++;

  Serial.println(F("\n--- [ DATA LOG MAP ROW ] ---"));
  Serial.print(F("CSV_LOG: "));
  Serial.print(pointCount);
  Serial.print(F(","));

  // 1. Capture and Print GPS Coordinates
  if (gps.location.isValid()) {
    Serial.print(gps.location.lat(), 6);
    Serial.print(F(","));
    Serial.print(gps.location.lng(), 6);
    Serial.print(F(","));
    Serial.print(gps.altitude.meters(), 2);
    Serial.print(F(","));
    Serial.print(gps.satellites.value());
    Serial.print(F(","));

    if (gps.hdop.isValid()) {
      Serial.print(gps.hdop.hdop(), 2);
    } else {
      Serial.print(F("N/A"));
    }
  } else {
    Serial.print(F("NO_GPS_FIX,NO_GPS_FIX,0.0,0,N/A"));
  }
  
  Serial.print(F(",NPK_DATA: "));

  // 2. Query and Print RS485 NPK Sensor Response
  readNPKSensor();
}

void readNPKSensor() {
  // 1. Switch MAX485 to TRANSMIT Mode
  digitalWrite(DE_RE_PIN, HIGH);
  delay(10);

  // 2. Transmit Modbus Command Frame
  Serial1.write(sensorRequest, sizeof(sensorRequest));
  Serial1.flush(); 

  // 3. Switch MAX485 back to RECEIVE Mode
  digitalWrite(DE_RE_PIN, LOW);

  // 4. Read Response Frame with 1-Second Timeout
  unsigned long startTime = millis();
  bool responseReceived = false;

  while (millis() - startTime < 1000) {
    if (Serial1.available()) {
      byte incomingByte = Serial1.read();
      if (incomingByte < 0x10) Serial.print("0");
      Serial.print(incomingByte, HEX);
      Serial.print(" ");
      responseReceived = true;
    }
  }

  if (!responseReceived) {
    Serial.println(F("[ERROR_TIMEOUT]"));
  } else {
    Serial.println();
  }
}

// =========================================================================
// DRIVETRAIN MOTOR CONTROL ROUTINES (BTS7960)
// =========================================================================

void motorForward() {
  analogWrite(L_LPWM, 0); analogWrite(R_LPWM, 0);
  analogWrite(L_RPWM, MOTOR_SPEED); analogWrite(R_RPWM, MOTOR_SPEED);
}

void motorBackward() {
  analogWrite(L_RPWM, 0); analogWrite(R_RPWM, 0);
  analogWrite(L_LPWM, MOTOR_SPEED); analogWrite(R_LPWM, MOTOR_SPEED);
}

void motorLeft() {
  analogWrite(L_RPWM, 0); analogWrite(R_RPWM, 0);
  analogWrite(L_LPWM, MOTOR_SPEED); analogWrite(R_RPWM, MOTOR_SPEED);
}

void motorRight() {
  analogWrite(L_RPWM, 0); analogWrite(L_LPWM, 0);
  analogWrite(L_RPWM, MOTOR_SPEED); analogWrite(R_LPWM, MOTOR_SPEED);
}

void motorStop() {
  analogWrite(L_RPWM, 0); analogWrite(L_LPWM, 0);
  analogWrite(R_RPWM, 0); analogWrite(R_LPWM, 0);
}