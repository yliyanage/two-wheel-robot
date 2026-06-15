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
 *   - 'S' is an explicit emergency stop. Unrecognized input is IGNORED (motor
 *     EMI can inject noise bytes on the serial RX while driving; ignoring them
 *     keeps a deliberate drive alive, and the watchdog still guarantees a stop).
 *
 * Serial commands (9600 baud, newline-terminated):
 *   F [pwm]   drive both wheels forward (pwm optional, default DEFAULT_PWM)
 *   M l r     manual: signed PWM per motor (+forward / -reverse), e.g. "M 80 -80"
 *   S         stop everything (motors off, driver standby, balance OFF)
 *
 *   --- self-balancing (ELEGOO Tumbller method, wheels ON the ground) ---
 *   B 1 / B 0 enable / disable the self-balancing controller (B alone toggles)
 *   C         capture the current pose as upright (sets the balance zero)
 *   D f t     balance disturbance: f = forward/back speed setpoint,
 *             t = turn bias. Auto-centres after MOVE_MS (bounded disturbance).
 *   K p d     set the balance (angle) ring gains: kp_balance kd_balance
 *   V p i     set the speed ring gains: kp_speed ki_speed
 *   T p d     set the turn ring gains: kp_turn kd_turn
 *   N kn ks l p   set the NONLINEAR balance layer: kp_nl k_smc lambda phi
 *             (gain-scheduling slope, sliding-mode gain, surface slope,
 *              boundary-layer width). kp_nl=0 & k_smc=0 -> pure linear ELEGOO.
 *   Z trim    set the upright trim angle in deg (mechanical zero offset)
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
const int PIN_VBAT      = A2;   // battery sense (Tumbller VOL_MEASURE_PIN)

// ---- Battery monitor (stock Tumbller method) -----------------------------
// The pack feeds pin A2 through a 10k/1.5k divider. Read it against the ATmega's
// internal 1.1 V reference (set in setup) so the ADC is accurate at low cell
// voltage. Vbatt = adc * (1.1/1024) * ((10+1.5)/1.5). Sampled once a second so
// it never competes with the 200 Hz balance loop. The brownout we diagnosed
// shows up here as a sagging voltage under load.
const float VBAT_ADC_VREF = 1.1f;             // internal reference (V)
const float VBAT_DIVIDER  = (10.0f + 1.5f) / 1.5f;   // 10k/1.5k divider ratio
const float VBAT_FULL     = 8.4f;             // 2S Li-ion fully charged (V)
const float VBAT_EMPTY    = 6.6f;             // treat as ~0% (sag/cutoff) (V)
const unsigned long VBAT_INTERVAL_MS = 1000;  // measure cadence
float         vbat = 0.0f;                    // last battery voltage (V)
unsigned long lastVbat = 0;

// Read the battery voltage on A2 and convert to volts using the divider.
void voltageMeasure() {
  int adc = analogRead(PIN_VBAT);
  vbat = (adc * VBAT_ADC_VREF / 1024.0f) * VBAT_DIVIDER;
}

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

// FORWARD direction = both DIR pins LOW; REVERSE = the opposite level.
const int FWD_LEFT  = LOW;
const int FWD_RIGHT = LOW;
const int REV_LEFT  = HIGH;
const int REV_RIGHT = HIGH;

// ---- Safety limits -------------------------------------------------------
const int           PWM_MAX     = 120;   // hard cap on MANUAL commanded speed
const int           DEFAULT_PWM = 80;    // gentle default
const unsigned long RUN_MAX_MS  = 3000;  // watchdog: auto-stop manual drive

// ---- Self-balancing config (ELEGOO Tumbller method) ----------------------
// Faithful port of the stock ELEGOO Tumbller V1.1 controller:
//   - 200 Hz (5 ms) control loop
//   - 1-D Kalman filter fuses accel pitch + gyro rate into a drift-free angle
//   - cascaded 3-ring controller: balance (PD on angle) + speed (PI on encoder
//     velocity, every 8th cycle = 40 ms) + turn (P/D on gyro Z)
//   - pwm_left  = balance - speed - turn ; pwm_right = balance - speed + turn
// Default gains are ELEGOO's shipped values; tune live with 'K' / 'V' / 'T'.
const int           BAL_DT_MS    = 5;     // 200 Hz control loop (ELEGOO uses 5 ms)
const float         BAL_DT       = 0.005; // seconds, matches BAL_DT_MS
const int           BAL_PWM_MAX  = 255;   // balancing may use the full range
const float         FALL_ANGLE   = 22.0;  // |tilt|>this = picked up/fallen -> stop
const unsigned long MOVE_MS      = 300;   // a 'D' nudge auto-centres after this
// Slew-rate limit: max change in motor PWM per 5 ms balance cycle. Stops the
// controller from slamming 0 -> +/-255 instantly, which stalls the motors and
// browns out the Nano (a reset wipes the live gains mid-test). 18/cycle still
// reaches full output in ~70 ms - fast enough to catch a fall, gentle on the
// battery. Raise once balancing is reliable.
const int           BAL_SLEW     = 18;
// Number of stationary samples averaged at boot / on 'C' to learn the gyro
// bias and seed the upright angle from the accelerometer.
const int           IMU_CAL_N    = 150;

// ELEGOO factory gains (BalanceCar.h). 55 balances a stock Tumbller, but with
// no slew limit it bang-bangs and browns out on this battery, so we bring up at
// gentler values and a softer speed ring. Tune live (K / V / T) once stable.
float kp_balance = 32.0, kd_balance = 0.95;   // angle ring (PD)
float kp_speed   = 6.0,  ki_speed   = 0.16;   // speed ring (PI)
float kp_turn    = 2.5,  kd_turn    = 0.5;    // turn ring
float angle_zero        = 0.0;   // mechanical upright offset (deg), set by 'Z'/'C'
float angular_velocity_zero = 0.0;

// ---- Nonlinear balance layer (gain-scheduling + sliding-mode) ------------
// Layered ON TOP of the linear ELEGOO PD balance ring. The two proven failure
// modes of the linear controller here were (a) a loose, underdamped wander
// near upright and (b) saturation/violence on a real fall. The nonlinear layer
// addresses both without abandoning the known-good cascade:
//   * Gain scheduling: kp_eff = kp_balance + kp_nl*|e|  -> soft near upright
//     (kills the jitter), firmer as the tilt grows (catches a fall sooner).
//   * Boundary-layer sliding mode: define the surface s = e_dot + lambda*e and
//     add u_smc = k_smc * sat(s/phi). sat() (not sign()) keeps a chatter-free
//     boundary layer of half-width phi so the motors don't buzz at standstill.
// Set kp_nl=0 AND k_smc=0 to recover the EXACT linear ELEGOO controller. Tune
// live with 'N kp_nl k_smc lambda phi'. Defaults are deliberately conservative.
float kp_nl      = 0.8;   // gain-schedule slope (PWM per deg per deg of |e|)
float k_smc      = 8.0;   // sliding-mode term magnitude (PWM); 0 = layer off
float lambda_smc = 0.6;   // sliding-surface slope: s = e_dot + lambda*e
float phi_smc    = 6.0;   // boundary-layer half-width (deg/s); 0 -> hard sign()
float smc_out    = 0.0;   // last sliding-mode contribution (telemetry only)

// If the robot drives the WRONG way when it tilts (accelerates the fall instead
// of catching it), flip MOTOR_SIGN. ELEGOO's wiring balances at +1; this lets
// you correct a mirrored motor/IMU mounting without re-deriving the math.
const float MOTOR_SIGN = 1.0;

bool  balancing   = false;
// Kalman filter state (ported from ELEGOO KalmanFilter.cpp)
float kal_angle   = 0.0;   // fused tilt angle (deg), 0 = upright
float kal_gyro_x  = 0.0;   // pitch-axis rate used by the balance ring (deg/s)
float kal_gyro_z  = 0.0;   // yaw rate used by the turn ring (deg/s)
float q_bias      = 0.0;   // estimated gyro bias
float angle_err   = 0.0, pCov[2][2] = {{1,0},{0,1}};
const float Q_angle = 0.001, Q_gyro = 0.005, R_angle = 0.5, C_0 = 1.0;
float gyro_x_bias = 0.0;   // learned at boot/'C' so the balance rate is zeroed

// Speed ring state
float speed_filter = 0.0, speed_filter_old = 0.0, car_speed_integral = 0.0;
float setting_car_speed = 0.0;   // forward/back setpoint (encoder counts/window)
float setting_turn_speed = 0.0;  // turn setpoint (PWM bias)
volatile long encL_speed = 0, encR_speed = 0;   // per-window encoder accumulators
uint8_t speed_period = 0;

int   pwm_left = 0, pwm_right = 0;
int   pwm_left_prev = 0, pwm_right_prev = 0;   // for the slew-rate limiter
unsigned long moveUntil = 0;
unsigned long lastBal   = 0;
bool  calibrated = false;

// ---- State ---------------------------------------------------------------
volatile long encLeft  = 0;
volatile long encRight = 0;
bool          driving      = false;
unsigned long driveStarted = 0;
int           curPwmL       = 0;   // signed: + = forward, - = reverse
int           curPwmR       = 0;
unsigned long lastTlm       = 0;
long          lastEncL      = 0;
long          lastEncR      = 0;

void onEncLeft() { encLeft++; encL_speed += (pwm_left < 0) ? -1 : 1; }

// Pin-change interrupt for the right encoder on pin 4 (PORTD bit 4).
// Pin 4 is NOT an external-interrupt pin, so we use PCINT instead.
volatile uint8_t lastPD = 0;
ISR(PCINT2_vect) {
  uint8_t now = PIND;
  if ((now ^ lastPD) & (1 << PD4)) {        // edge on pin 4
    encRight++;
    encR_speed += (pwm_right < 0) ? -1 : 1; // sign by motor direction (ELEGOO)
  }
  lastPD = now;
}

void allStop() {
  analogWrite(PIN_PWM_LEFT, 0);
  analogWrite(PIN_PWM_RIGHT, 0);
  digitalWrite(PIN_STBY, LOW);   // driver standby = nothing can move
  digitalWrite(PIN_LED, LOW);
  driving = false;
  balancing = false;             // stopping always leaves balancing OFF
  car_speed_integral = 0;
  speed_filter = 0; speed_filter_old = 0;
  setting_car_speed = 0; setting_turn_speed = 0;
  pwm_left = 0; pwm_right = 0;
  pwm_left_prev = 0; pwm_right_prev = 0;
  curPwmL = 0;
  curPwmR = 0;
}

// Low-level: push signed PWM to each motor, clamped to +/- cap. Sets the
// driver out of standby. Updates curPwmL/R for telemetry. Does NOT touch the
// manual watchdog (callers decide the safety policy).
void setMotors(int left, int right, int cap) {
  left  = constrain(left,  -cap, cap);
  right = constrain(right, -cap, cap);

  digitalWrite(PIN_DIR_LEFT,  left  >= 0 ? FWD_LEFT  : REV_LEFT);
  digitalWrite(PIN_DIR_RIGHT, right >= 0 ? FWD_RIGHT : REV_RIGHT);
  digitalWrite(PIN_STBY, HIGH);          // enable driver
  analogWrite(PIN_PWM_LEFT,  abs(left));
  analogWrite(PIN_PWM_RIGHT, abs(right));
  curPwmL = left;
  curPwmR = right;
}

// Drive each motor independently (MANUAL mode). Values are signed PWM:
//   > 0 -> that motor's FORWARD direction, < 0 -> REVERSE, 0 -> stopped.
// Magnitudes are clamped to PWM_MAX; the watchdog still auto-stops after RUN_MAX_MS.
void driveMotors(int left, int right) {
  setMotors(left, right, PWM_MAX);
  driving      = (curPwmL != 0 || curPwmR != 0);
  digitalWrite(PIN_LED, driving ? HIGH : LOW);
  driveStarted = millis();
}

// Both wheels forward at the same speed (kept for the simple 'F' command).
void driveForward(int pwm) {
  if (pwm < 0) pwm = 0;
  driveMotors(pwm, pwm);
}

// Average IMU_CAL_N stationary samples to (a) learn the gyro X bias so the
// balance rate reads ~0 when still, and (b) seed the Kalman angle from the
// accelerometer so balancing starts from the true tilt instead of 0. The robot
// must be held still (ideally upright) while this runs (~0.3 s).
void calibrateImu() {
  long gxSum = 0;
  float accSum = 0;
  int got = 0;
  for (int i = 0; i < IMU_CAL_N; i++) {
    if (mpuRead()) {
      gxSum  += gx;
      accSum += atan2((float)ay, (float)az) * 57.2958;
      got++;
    }
    delay(2);
  }
  if (got > 0) {
    gyro_x_bias = (gxSum / (float)got) / 131.0;   // deg/s offset
    kal_angle   = accSum / got;                    // seed fused angle
    q_bias      = 0.0;
    pCov[0][0] = 1; pCov[0][1] = 0; pCov[1][0] = 0; pCov[1][1] = 1;
  }
}

// Capture the current pose as "upright" (tilt = 0): re-learn the gyro bias and
// seed the angle, then set angle_zero to the current Kalman angle so the
// balance ring holds this exact lean.
void calibrateUpright() {
  calibrateImu();
  angle_zero = kal_angle;
  car_speed_integral = 0;
  calibrated = true;
}

// 1-D Kalman filter (ported from ELEGOO KalmanFilter.cpp). Fuses the accel
// pitch (absolute but noisy) with the integrated gyro rate (smooth but drifts)
// into kal_angle, while estimating the gyro bias q_bias.
void kalmanUpdate(float accAngle, float gyroRate, float dt) {
  kal_angle += (gyroRate - q_bias) * dt;
  angle_err  = accAngle - kal_angle;

  float Pdot0 = Q_angle - pCov[0][1] - pCov[1][0];
  float Pdot1 = -pCov[1][1];
  float Pdot2 = -pCov[1][1];
  float Pdot3 = Q_gyro;
  pCov[0][0] += Pdot0 * dt;
  pCov[0][1] += Pdot1 * dt;
  pCov[1][0] += Pdot2 * dt;
  pCov[1][1] += Pdot3 * dt;

  float E = R_angle + C_0 * pCov[0][0];
  float K0 = C_0 * pCov[0][0] / E;
  float K1 = C_0 * pCov[1][0] / E;

  float t0 = pCov[0][0], t1 = pCov[0][1];
  pCov[0][0] -= K0 * t0;
  pCov[0][1] -= K0 * t1;
  pCov[1][0] -= K1 * t0;
  pCov[1][1] -= K1 * t1;

  kal_angle += K0 * angle_err;
  q_bias    += K1 * angle_err;
}

// Run one 200 Hz step of the ELEGOO cascaded controller. Always refreshes the
// Kalman angle; when balancing, runs the balance + speed + turn rings.
void balanceStep() {
  // --- read IMU and update the Kalman tilt estimate ---
  if (mpuOk) mpuOk = mpuRead();
  // ELEGOO uses pitch from the Y/Z accel axes and Gyro_x as the balance rate.
  float accAngle = mpuOk ? atan2((float)ay, (float)az) * 57.2958 : kal_angle;
  kal_gyro_x = mpuOk ? (gx / 131.0) - gyro_x_bias : 0.0;   // bias-corrected rate
  kal_gyro_z = mpuOk ? -gz / 131.0 : 0.0;  // yaw rate for the turn ring
  kalmanUpdate(accAngle, kal_gyro_x, BAL_DT);

  if (!balancing) { car_speed_integral = 0; pwm_left_prev = 0; pwm_right_prev = 0; return; }

  // --- a 'D' nudge auto-centres so each press is a bounded disturbance ---
  if (moveUntil && millis() >= moveUntil) { setting_car_speed = 0; setting_turn_speed = 0; moveUntil = 0; }

  // --- fall / pick-up detection: stop if tilted past the balance window ---
  if (fabs(kal_angle - angle_zero) > FALL_ANGLE) {
    allStop();
    Serial.println(F("STOP fell"));
    return;
  }

  // --- balance ring: nonlinear PD (gain-scheduled) + sliding-mode term ---
  // e = tilt error (deg), e_dot = tilt rate (deg/s). Both share the same sign
  // convention as the linear ELEGOO ring, so every term pushes the same way.
  float e     = kal_angle - angle_zero;
  float e_dot = kal_gyro_x - angular_velocity_zero;
  // Gain scheduling: gentle near upright, firmer on larger tilt.
  float kp_eff = kp_balance + kp_nl * fabs(e);
  // Boundary-layer sliding mode: drive the surface s -> 0 without chattering.
  float s   = e_dot + lambda_smc * e;
  float sat = (phi_smc > 0.0001) ? constrain(s / phi_smc, -1.0, 1.0)
                                 : (float)((s > 0) - (s < 0));
  smc_out = k_smc * sat;
  float balance_output = kp_eff * e + kd_balance * e_dot + smc_out;

  // --- speed ring: PI on encoder velocity, updated every 8th cycle (40 ms) ---
  if (++speed_period >= 8) {
    speed_period = 0;
    long el, er;
    noInterrupts(); el = encL_speed; er = encR_speed; encL_speed = 0; encR_speed = 0; interrupts();
    float car_speed = (el + er) * 0.5;
    speed_filter = speed_filter_old * 0.7 + car_speed * 0.3;
    speed_filter_old = speed_filter;
    car_speed_integral += speed_filter;
    car_speed_integral += -setting_car_speed;
    car_speed_integral = constrain(car_speed_integral, -3000, 3000);
  }
  // ELEGOO computes speed_output POSITIVE and SUBTRACTS it below, so the speed
  // ring DAMPS translation. (A leading-negative here would make pwm = balance +
  // speed = positive feedback -> the robot drives away faster and faster.)
  float speed_output = kp_speed * speed_filter + ki_speed * car_speed_integral;

  // --- turn ring: setpoint + D on yaw rate ---
  float turn_output = setting_turn_speed + kd_turn * kal_gyro_z;

  // --- combine: ELEGOO sign convention ---
  pwm_left  = (int)(MOTOR_SIGN * (balance_output - speed_output - turn_output));
  pwm_right = (int)(MOTOR_SIGN * (balance_output - speed_output + turn_output));
  pwm_left  = constrain(pwm_left,  -BAL_PWM_MAX, BAL_PWM_MAX);
  pwm_right = constrain(pwm_right, -BAL_PWM_MAX, BAL_PWM_MAX);

  // --- slew-rate limit: ramp toward the target so we never slam the motors
  //     (prevents the current spike that browns out the Nano) ---
  pwm_left  = pwm_left_prev  + constrain(pwm_left  - pwm_left_prev,  -BAL_SLEW, BAL_SLEW);
  pwm_right = pwm_right_prev + constrain(pwm_right - pwm_right_prev, -BAL_SLEW, BAL_SLEW);
  pwm_left_prev  = pwm_left;
  pwm_right_prev = pwm_right;

  setMotors(pwm_left, pwm_right, BAL_PWM_MAX);
  digitalWrite(PIN_LED, HIGH);
}

void setup() {
  Serial.begin(9600);
  Serial.setTimeout(50);   // don't let a noisy/partial line stall the loop
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
  if (mpuOk) calibrateImu();   // learn gyro bias + seed angle while still at boot

  // Switch the ADC to the internal 1.1 V reference for accurate battery sensing
  // (the divider puts a full 8.4 V pack at ~1.1 V on A2). A throwaway read lets
  // the reference settle before the first real sample.
  analogReference(INTERNAL);
  analogRead(PIN_VBAT);
  voltageMeasure();
  Serial.println(F("# drive-forward ready. Commands: 'F [pwm]', 'M l r', 'S'. Keep wheels OFF ground."));
}

void loop() {
  // --- handle serial commands ---
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      char c = line.charAt(0);
      if (c == 'F' || c == 'f') {
        balancing = false;              // manual drive overrides balancing
        int pwm = DEFAULT_PWM;
        String arg = line.substring(1);
        arg.trim();
        if (arg.length() > 0) pwm = arg.toInt();
        driveForward(pwm);
        Serial.print(F("DRIVE forward pwm=")); Serial.println(curPwmL);
      } else if (c == 'M' || c == 'm') {
        // "M <left> <right>" - signed per-motor PWM (manual troubleshoot).
        balancing = false;              // manual drive overrides balancing
        String arg = line.substring(1);
        arg.trim();
        int sp = arg.indexOf(' ');
        int l = 0, r = 0;
        if (sp > 0) {
          l = arg.substring(0, sp).toInt();
          r = arg.substring(sp + 1).toInt();
        } else {
          l = arg.toInt();                // single value -> both motors
          r = l;
        }
        driveMotors(l, r);
        Serial.print(F("DRIVE L=")); Serial.print(curPwmL);
        Serial.print(F(" R="));      Serial.println(curPwmR);
      } else if (c == 'B' || c == 'b') {
        // "B 1" / "B 0" enable/disable balancing; bare "B" toggles.
        String arg = line.substring(1); arg.trim();
        bool want = (arg.length() == 0) ? !balancing : (arg.toInt() != 0);
        if (want) {
          driving = false;              // hand the motors to the balance loop
          calibrateUpright();           // always re-learn bias + upright on enable
          car_speed_integral = 0; speed_filter = 0; speed_filter_old = 0;
          setting_car_speed = 0; setting_turn_speed = 0; moveUntil = 0;
          pwm_left = 0; pwm_right = 0; pwm_left_prev = 0; pwm_right_prev = 0;
          noInterrupts(); encL_speed = 0; encR_speed = 0; interrupts();
          balancing = true;
          Serial.println(F("BALANCE on"));
        } else {
          allStop();
          Serial.println(F("BALANCE off"));
        }
      } else if (c == 'C' || c == 'c') {
        // "C" capture the current pose as upright (tilt zero).
        calibrateUpright();
        Serial.print(F("CAL upright zero=")); Serial.println(angle_zero);
      } else if (c == 'D' || c == 'd') {
        // "D <fwd> <turn>" balance disturbance (auto-centres after MOVE_MS):
        //   fwd  -> forward/back speed setpoint (encoder counts/window, +=ahead)
        //   turn -> turn bias (PWM, + = spin right)
        String arg = line.substring(1); arg.trim();
        int sp = arg.indexOf(' ');
        if (sp > 0) {
          setting_car_speed  = arg.substring(0, sp).toFloat();
          setting_turn_speed = arg.substring(sp + 1).toFloat();
        } else {
          setting_car_speed  = arg.toFloat();
          setting_turn_speed = 0;
        }
        moveUntil = millis() + MOVE_MS;
        Serial.print(F("MOVE fwd=")); Serial.print(setting_car_speed);
        Serial.print(F(" turn="));    Serial.println(setting_turn_speed);
      } else if (c == 'K' || c == 'k') {
        // "K <kp_balance> <kd_balance>" live tune of the angle ring.
        String arg = line.substring(1); arg.trim();
        int s1 = arg.indexOf(' ');
        if (s1 > 0) {
          kp_balance = arg.substring(0, s1).toFloat();
          kd_balance = arg.substring(s1 + 1).toFloat();
          Serial.print(F("BALGAINS kp=")); Serial.print(kp_balance);
          Serial.print(F(" kd="));         Serial.println(kd_balance);
        }
      } else if (c == 'V' || c == 'v') {
        // "V <kp_speed> <ki_speed>" live tune of the speed ring.
        String arg = line.substring(1); arg.trim();
        int s1 = arg.indexOf(' ');
        if (s1 > 0) {
          kp_speed = arg.substring(0, s1).toFloat();
          ki_speed = arg.substring(s1 + 1).toFloat();
          car_speed_integral = 0;
          Serial.print(F("SPDGAINS kp=")); Serial.print(kp_speed);
          Serial.print(F(" ki="));         Serial.println(ki_speed);
        }
      } else if (c == 'T' || c == 't') {
        // "T <kp_turn> <kd_turn>" live tune of the turn ring.
        String arg = line.substring(1); arg.trim();
        int s1 = arg.indexOf(' ');
        if (s1 > 0) {
          kp_turn = arg.substring(0, s1).toFloat();
          kd_turn = arg.substring(s1 + 1).toFloat();
          Serial.print(F("TURNGAINS kp=")); Serial.print(kp_turn);
          Serial.print(F(" kd="));          Serial.println(kd_turn);
        }
      } else if (c == 'N' || c == 'n') {
        // "N <kp_nl> <k_smc> <lambda> <phi>" live tune of the nonlinear layer.
        // Any trailing args may be omitted; only the ones supplied are changed.
        String arg = line.substring(1); arg.trim();
        float vals[4]; int n = 0;
        while (arg.length() > 0 && n < 4) {
          int sp = arg.indexOf(' ');
          String tok = (sp < 0) ? arg : arg.substring(0, sp);
          vals[n++] = tok.toFloat();
          if (sp < 0) break;
          arg = arg.substring(sp + 1); arg.trim();
        }
        if (n >= 1) kp_nl      = vals[0];
        if (n >= 2) k_smc      = vals[1];
        if (n >= 3) lambda_smc = vals[2];
        if (n >= 4) phi_smc    = vals[3];
        Serial.print(F("NLGAINS kp_nl=")); Serial.print(kp_nl);
        Serial.print(F(" ksmc="));   Serial.print(k_smc);
        Serial.print(F(" lambda=")); Serial.print(lambda_smc);
        Serial.print(F(" phi="));    Serial.println(phi_smc);
      } else if (c == 'Z' || c == 'z') {
        // "Z <deg>" set the upright trim (mechanical zero offset, deg).
        String arg = line.substring(1); arg.trim();
        angle_zero = arg.toFloat();
        Serial.print(F("TRIM ")); Serial.println(angle_zero);
      } else if (c == 'S' || c == 's') {
        allStop();
        Serial.println(F("STOP"));
      } else {
        // Unrecognized input - almost always electrical noise injected on the
        // serial RX while the motors run (TB6612 / motor EMI). Ignore it so a
        // stray byte cannot kill a deliberate drive. Safety still holds: 'S'
        // above is the explicit stop and the RUN_MAX_MS watchdog auto-stops.
      }
    }
  }

  // --- watchdog: never let the wheels run unattended (MANUAL drive only) ---
  if (driving && !balancing && (millis() - driveStarted >= RUN_MAX_MS)) {
    allStop();
    Serial.println(F("STOP watchdog"));
  }

  // --- 200 Hz balance / Kalman loop (ELEGOO uses 5 ms) ---
  unsigned long tnow = millis();
  if (tnow - lastBal >= (unsigned long)BAL_DT_MS) {
    lastBal = tnow;
    balanceStep();
  }

  // --- battery voltage at 1 Hz (cheap, never competes with the balance loop) ---
  if (tnow - lastVbat >= VBAT_INTERVAL_MS) {
    lastVbat = tnow;
    voltageMeasure();
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

    // NOTE: do NOT read the MPU here - balanceStep() at 200 Hz already keeps
    // ax..gz and kal_angle fresh; a second read would race it.
    float angle = kal_angle;               // Kalman-fused tilt angle
    bool  active = driving || balancing;   // motors live in either mode

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
    Serial.print(F(" pwmL="));    Serial.print(active ? curPwmL : 0);
    Serial.print(F(" pwmR="));    Serial.print(active ? curPwmR : 0);
    Serial.print(F(" stby="));    Serial.print(active ? 0 : 1);
    Serial.print(F(" bal="));     Serial.print(balancing ? 1 : 0);
    Serial.print(F(" tgt="));     Serial.print(angle_zero, 2);
    Serial.print(F(" kp="));      Serial.print(kp_balance, 2);
    Serial.print(F(" ki="));      Serial.print(ki_speed, 2);
    Serial.print(F(" kd="));      Serial.print(kd_balance, 2);
    Serial.print(F(" trim="));    Serial.print(angle_zero, 2);
    Serial.print(F(" knl="));     Serial.print(kp_nl, 2);
    Serial.print(F(" ksmc="));    Serial.print(k_smc, 2);
    Serial.print(F(" smc="));     Serial.print(smc_out, 1);
    Serial.print(F(" vbat="));    Serial.print(vbat, 2);
    Serial.print(F(" mpu="));     Serial.println(mpuOk ? 1 : 0);
  }
}
