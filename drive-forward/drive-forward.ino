/*
 * drive-forward.ino  -  drive BOTH Tumbller motors forward (bench-safe)
 *
 * CONFIRMED pin map (verified with pin-probe):
 *   STBY (driver enable) = 8   -> HIGH to allow motion
 *   Left  motor : PWM = 5, DIR = 7
 *   Right motor : PWM = 6, DIR = 12
 *   Left encoder = 2 (INT0).   Right encoder is DEAD (reads 0).
 *   FORWARD = both DIR pins LOW.
 *
 * SAFETY (this is a self-balancing robot, keep wheels OFF the ground):
 *   - PWM is capped at PWM_MAX.
 *   - A watchdog auto-stops the motors after RUN_MAX_MS unless re-commanded,
 *     so a lost serial link cannot leave the wheels spinning.
 *   - 'S' (or any unknown input) is an immediate emergency stop.
 *
 * Serial commands (9600 baud, newline-terminated):
 *   F [pwm]   drive both wheels forward (pwm optional, default DEFAULT_PWM)
 *   S         stop (motors off, driver standby)
 */

#include <Arduino.h>
#include <Wire.h>

// ---- Confirmed pins ------------------------------------------------------
const int PIN_STBY      = 8;
const int PIN_PWM_LEFT  = 5;
const int PIN_DIR_LEFT  = 7;
const int PIN_PWM_RIGHT = 6;
const int PIN_DIR_RIGHT = 12;
const int PIN_ENC_LEFT  = 2;   // INT0 (hardware interrupt)
const int PIN_ENC_RIGHT = 4;   // PCINT20 (pin-change interrupt, PORTD bit 4)
const int PIN_LED       = 13;

// ---- MPU6050 (raw I2C, no library) ---------------------------------------
#define MPU_ADDR 0x68
bool    mpuOk = false;
int16_t ax, ay, az, gx, gy, gz;

bool mpuBegin() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0x00);     // wake the sensor
  return (Wire.endTransmission() == 0);
}

bool mpuRead() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);                        // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, 14, true) != 14) return false;
  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();                // temperature (skip)
  gx = (Wire.read() << 8) | Wire.read();
  gy = (Wire.read() << 8) | Wire.read();
  gz = (Wire.read() << 8) | Wire.read();
  return true;
}

// FORWARD direction = both DIR pins LOW.
const int FWD_LEFT  = LOW;
const int FWD_RIGHT = LOW;

// ---- Safety limits -------------------------------------------------------
const int           PWM_MAX     = 120;   // hard cap on commanded speed
const int           DEFAULT_PWM = 80;    // gentle default
const unsigned long RUN_MAX_MS  = 3000;  // watchdog: auto-stop after this

// ---- State ---------------------------------------------------------------
volatile long encLeft  = 0;
volatile long encRight = 0;
bool          driving      = false;
unsigned long driveStarted = 0;
int           curPwm        = 0;
unsigned long lastTlm       = 0;
long          lastEncL      = 0;
long          lastEncR      = 0;

void onEncLeft() { encLeft++; }

// Pin-change interrupt for the right encoder on pin 4 (PORTD bit 4).
// Pin 4 is NOT an external-interrupt pin, so we use PCINT instead.
volatile uint8_t lastPD = 0;
ISR(PCINT2_vect) {
  uint8_t now = PIND;
  if ((now ^ lastPD) & (1 << PD4)) encRight++;   // edge on pin 4
  lastPD = now;
}

void allStop() {
  analogWrite(PIN_PWM_LEFT, 0);
  analogWrite(PIN_PWM_RIGHT, 0);
  digitalWrite(PIN_STBY, LOW);   // driver standby = nothing can move
  digitalWrite(PIN_LED, LOW);
  driving = false;
  curPwm  = 0;
}

void driveForward(int pwm) {
  if (pwm < 0)       pwm = 0;
  if (pwm > PWM_MAX) pwm = PWM_MAX;

  digitalWrite(PIN_DIR_LEFT,  FWD_LEFT);
  digitalWrite(PIN_DIR_RIGHT, FWD_RIGHT);
  digitalWrite(PIN_STBY, HIGH);          // enable driver
  analogWrite(PIN_PWM_LEFT,  pwm);
  analogWrite(PIN_PWM_RIGHT, pwm);
  digitalWrite(PIN_LED, HIGH);

  driving      = true;
  curPwm       = pwm;
  driveStarted = millis();
}

void setup() {
  Serial.begin(9600);
  Wire.begin();

  pinMode(PIN_STBY,      OUTPUT);
  pinMode(PIN_PWM_LEFT,  OUTPUT);
  pinMode(PIN_DIR_LEFT,  OUTPUT);
  pinMode(PIN_PWM_RIGHT, OUTPUT);
  pinMode(PIN_DIR_RIGHT, OUTPUT);
  pinMode(PIN_LED,       OUTPUT);
  pinMode(PIN_ENC_LEFT,  INPUT);

  allStop();                              // start in the safe state
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_LEFT), onEncLeft, CHANGE);

  // Enable pin-change interrupt on pin 4 (PCINT20) for the right encoder.
  pinMode(PIN_ENC_RIGHT, INPUT);
  lastPD  = PIND;
  PCICR  |= (1 << PCIE2);     // enable PORTD pin-change interrupts
  PCMSK2 |= (1 << PCINT20);   // unmask pin 4

  mpuOk = mpuBegin();

  Serial.println(F("# drive-forward ready. Commands: 'F [pwm]', 'S'. Keep wheels OFF ground."));
}

void loop() {
  // --- handle serial commands ---
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      char c = line.charAt(0);
      if (c == 'F' || c == 'f') {
        int pwm = DEFAULT_PWM;
        String arg = line.substring(1);
        arg.trim();
        if (arg.length() > 0) pwm = arg.toInt();
        driveForward(pwm);
        Serial.print(F("DRIVE forward pwm=")); Serial.println(curPwm);
      } else {
        allStop();                        // 'S' or anything unexpected
        Serial.println(F("STOP"));
      }
    }
  }

  // --- watchdog: never let the wheels run unattended ---
  if (driving && (millis() - driveStarted >= RUN_MAX_MS)) {
    allStop();
    Serial.println(F("STOP watchdog"));
  }

  // --- full telemetry at 5 Hz (matches the dashboard format) ---
  unsigned long now = millis();
  if (now - lastTlm >= 200) {
    float dt = (now - lastTlm) / 1000.0;
    lastTlm = now;

    long e;
    noInterrupts(); e = encLeft; interrupts();
    float spdL = dt > 0 ? (e - lastEncL) / dt : 0;
    lastEncL = e;

    long er;
    noInterrupts(); er = encRight; interrupts();
    float spdR = dt > 0 ? (er - lastEncR) / dt : 0;
    lastEncR = er;

    if (mpuOk) mpuOk = mpuRead();
    float angle = mpuOk ? atan2((float)ax, (float)az) * 57.2958 : 0.0;

    // stby=1 means driver disabled.
    Serial.print(F("TLM t="));    Serial.print(now);
    Serial.print(F(" encL="));    Serial.print(e);
    Serial.print(F(" encR="));    Serial.print(er);
    Serial.print(F(" spdL="));    Serial.print(spdL, 1);
    Serial.print(F(" spdR="));    Serial.print(spdR, 1);
    Serial.print(F(" ax="));      Serial.print(ax);
    Serial.print(F(" ay="));      Serial.print(ay);
    Serial.print(F(" az="));      Serial.print(az);
    Serial.print(F(" gx="));      Serial.print(gx);
    Serial.print(F(" gy="));      Serial.print(gy);
    Serial.print(F(" gz="));      Serial.print(gz);
    Serial.print(F(" angle="));   Serial.print(angle, 2);
    Serial.print(F(" pwmL="));    Serial.print(driving ? curPwm : 0);
    Serial.print(F(" pwmR="));    Serial.print(driving ? curPwm : 0);
    Serial.print(F(" stby="));    Serial.print(driving ? 0 : 1);
    Serial.print(F(" mpu="));     Serial.println(mpuOk ? 1 : 0);
  }
}
