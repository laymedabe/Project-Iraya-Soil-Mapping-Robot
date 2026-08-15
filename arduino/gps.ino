#include <SoftwareSerial.h>
#include <TinyGPS++.h>

// Software Serial Pins
static const int RXPin = 4; // Arduino Pin 4 -> GPS TX
static const int TXPin = 3; // Arduino Pin 3 -> GPS RX
static const uint32_t GPSBaud = 9600;

TinyGPSPlus gps;
SoftwareSerial gpsSerial(RXPin, TXPin);

int pointCount = 0;

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(GPSBaud);

  Serial.println(F("===================================================="));
  Serial.println(F("          GPS POSITION DATA COLLECTOR               "));
  Serial.println(F("===================================================="));
  Serial.println(F("Instructions: Type 'c' in Serial Monitor & hit Enter "));
  Serial.println(F("              to capture your current position.    "));
  Serial.println(F("===================================================="));
  Serial.println();
  
  // CSV Header Line
  Serial.println(F("Point_ID,Latitude,Longitude,Altitude_m,Satellites,HDOP"));
}

void loop() {
  // Read incoming stream from GPS module
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // Check if user typed 'c' in the Serial Monitor to record a point
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == 'c' || cmd == 'C') {
      capturePosition();
    }
  }
}

void capturePosition() {
  // Ensure we have a valid GPS fix before logging
  if (gps.location.isValid()) {
    pointCount++;
    
    Serial.print(pointCount);
    Serial.print(F(","));
    Serial.print(gps.location.lat(), 6);
    Serial.print(F(","));
    Serial.print(gps.location.lng(), 6);
    Serial.print(F(","));
    Serial.print(gps.altitude.meters(), 2);
    Serial.print(F(","));
    Serial.print(gps.satellites.value());
    Serial.print(F(","));
    
    // HDOP (Horizontal Dilution of Precision): Lower value = higher accuracy (< 2.0 is ideal)
    if (gps.hdop.isValid()) {
      Serial.println(gps.hdop.hdop(), 2);
    } else {
      Serial.println(F("N/A"));
    }
  } else {
    Serial.println(F("[ERROR] Cannot log point: No valid 3D GPS fix acquired yet."));
  }
}