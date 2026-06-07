/*
 * tumbller-robot.ino  -  starter sketch for the Elegoo Tumbller (Arduino Nano)
 *
 * Hardware on this robot (confirmed from the stock firmware's serial output):
 *   - 2x DC gear motors, each with a quadrature wheel encoder
 *   - TB6612FNG dual motor driver
 *   - MPU6050 gyro/accel on I2C (A4=SDA, A5=SCL)  [used for self-balancing]
 *
 * SAFETY: This starter keeps the motor driver in STANDBY (motors OFF). It only
 * blinks the LED and prints encoder counts, so it cannot make the robot run
 * away while we verify wiring. We enable motor/balance control in later steps.
 *
 * !!! VERIFY PINS BEFORE ENABLING MOTORS !!!
 * The pin numbers below are the documented Elegoo Tumbller defaults, but they
 * MUST be confirmed against your board's manual/source before driving motors.
 * Wrong pins on a balancing bot = uncontrolled motion.
 */

#include <Wire.h>

// ---- Pin map (Elegoo Tumbller defaults - VERIFY) -------------------------
#define PIN_ENCODER_LEFT     2   // INT0 - left encoder pulse
#define PIN_ENCODER_RIGHT    4   // left/right encoder pulse (4 & 2 pair on Tumbller)
#define PIN_MOTOR_PWM_LEFT   6   // PWM speed, left motor  (TB6612 PWMA)
#define PIN_MOTOR_PWM_RIGHT  5   // PWM speed, right motor (TB6612 PWMB)
#define PIN_MOTOR_AIN1       7   // direction, left motor  (TB6612 AIN1)
#define PIN_MOTOR_BIN1       8   // direction, right motor (TB6612 BIN1)
#define PIN_MOTOR_STBY       3   // TB6612 STBY: LOW = motors disabled (safe)
#define PIN_LED              13  // onboard LED

volatile long encoder_count_left_a  = 0;
volatile long encoder_count_right_a = 0;

void countLeft()  { encoder_count_left_a++;  }
void countRight() { encoder_count_right_a++; }

// ---- MPU6050 (raw I2C reads, no external library) ------------------------
#define MPU_ADDR 0x68
bool    mpuOk = false;
int16_t ax, ay, az, gx, gy, gz;

bool mpuBegin() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);            // PWR_MGMT_1
  Wire.write(0x00);            // wake the sensor
  return (Wire.endTransmission() == 0);
}

bool mpuRead() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);            // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, 14, true) != 14) return false;
  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();    // temperature (skip)
  gx = (Wire.read() << 8) | Wire.read();
  gy = (Wire.read() << 8) | Wire.read();
  gz = (Wire.read() << 8) | Wire.read();
  return true;
}

// ---- Telemetry timing ----------------------------------------------------
const unsigned long TLM_INTERVAL_MS = 100;   // 10 Hz
unsigned long lastTlm  = 0;
long          lastEncL = 0;
long          lastEncR = 0;

void setup() {
  Serial.begin(9600);
  Wire.begin();

  pinMode(PIN_LED, OUTPUT);

  // Force the motor driver into standby so nothing moves.
  pinMode(PIN_MOTOR_STBY, OUTPUT);
  digitalWrite(PIN_MOTOR_STBY, LOW);          // motors OFF
  pinMode(PIN_MOTOR_PWM_LEFT, OUTPUT);
  pinMode(PIN_MOTOR_PWM_RIGHT, OUTPUT);
  analogWrite(PIN_MOTOR_PWM_LEFT, 0);
  analogWrite(PIN_MOTOR_PWM_RIGHT, 0);

  // Encoders as inputs; attach interrupt to the one on a usable INT pin.
  pinMode(PIN_ENCODER_LEFT, INPUT);
  pinMode(PIN_ENCODER_RIGHT, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_LEFT), countLeft, CHANGE);

  mpuOk = mpuBegin();

  Serial.println(F("# tumbller telemetry ready (motors in STANDBY)"));
}

void loop() {
  unsigned long now = millis();
  if (now - lastTlm < TLM_INTERVAL_MS) return;
  float dt = (now - lastTlm) / 1000.0;
  lastTlm = now;

  // Atomically snapshot the interrupt-driven counters.
  long cl, cr;
  noInterrupts();
  cl = encoder_count_left_a;
  cr = encoder_count_right_a;
  interrupts();

  // Wheel speed in encoder counts/second.
  float spdL = dt > 0 ? (cl - lastEncL) / dt : 0;
  float spdR = dt > 0 ? (cr - lastEncR) / dt : 0;
  lastEncL = cl;
  lastEncR = cr;

  // Read the gyro/accel; approximate tilt (pitch) from the accelerometer.
  if (mpuOk) mpuOk = mpuRead();
  float angle = mpuOk ? atan2((float)ax, (float)az) * 57.2958 : 0.0;

  digitalWrite(PIN_LED, !digitalRead(PIN_LED));   // heartbeat

  // One machine-parseable line per cycle: "TLM key=value key=value ..."
  Serial.print(F("TLM t="));    Serial.print(now);
  Serial.print(F(" encL="));    Serial.print(cl);
  Serial.print(F(" encR="));    Serial.print(cr);
  Serial.print(F(" spdL="));    Serial.print(spdL, 1);
  Serial.print(F(" spdR="));    Serial.print(spdR, 1);
  Serial.print(F(" ax="));      Serial.print(ax);
  Serial.print(F(" ay="));      Serial.print(ay);
  Serial.print(F(" az="));      Serial.print(az);
  Serial.print(F(" gx="));      Serial.print(gx);
  Serial.print(F(" gy="));      Serial.print(gy);
  Serial.print(F(" gz="));      Serial.print(gz);
  Serial.print(F(" angle="));   Serial.print(angle, 2);
  Serial.print(F(" pwmL=0 pwmR=0 stby=1 mpu="));
  Serial.println(mpuOk ? 1 : 0);
}

