/*
 * pin-probe.ino  -  SAFE functional pin verification for the Tumbller (Nano)
 *
 * Purpose: prove which pins drive which motor (and which direction is "forward")
 * by pulsing ONE candidate (PWM pin + direction pin) combo at LOW power for a
 * short, bounded time, then reporting how much the LEFT encoder moved.
 *
 * SAFETY RAILS:
 *   - Nothing moves until an explicit serial command arrives.
 *   - PWM is hard-capped at PWM_MAX (low). Pulses auto-stop after the requested
 *     time, and never run longer than PULSE_MAX_MS.
 *   - "S" (or any unknown input) = EMERGENCY STOP: all candidate pins LOW.
 *   - Between pulses every candidate pin is driven LOW (motors idle).
 *
 * WHEELS MUST BE OFF THE GROUND.
 *
 * Serial command (9600 baud), space-separated, end with newline:
 *   T <pwmPin> <dirPin> <dirVal> <pwmVal> <ms> [stbyPin]
 *     pwmPin : candidate PWM pin   (e.g. 5 or 6)
 *     dirPin : candidate direction pin (e.g. 7, 8, or 12)
 *     dirVal : 0 or 1 (direction level to test)
 *     pwmVal : 0..PWM_MAX (clamped)
 *     ms     : pulse length, clamped to PULSE_MAX_MS
 *     stbyPin: OPTIONAL TB6612 STBY pin to hold HIGH during the pulse
 *   Example:  T 5 7 1 45 300 3
 *   Stop:     S
 */

#define PWM_MAX        70      // hard cap on test PWM (keep it gentle)
#define PULSE_MAX_MS   600     // hard cap on pulse duration
#define PIN_ENCODER_LEFT 2     // known-good left encoder (INT0)
#define PIN_LED         13

// Every pin we might touch while probing. All forced LOW when idle.
const uint8_t CANDIDATES[] = {3, 5, 6, 7, 8, 12};
const uint8_t N_CAND = sizeof(CANDIDATES);

volatile long encLeft = 0;
void countLeft() { encLeft++; }

void allLow() {
  for (uint8_t i = 0; i < N_CAND; i++) {
    pinMode(CANDIDATES[i], OUTPUT);
    digitalWrite(CANDIDATES[i], LOW);
  }
}

void setup() {
  Serial.begin(9600);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_ENCODER_LEFT, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_LEFT), countLeft, CHANGE);
  allLow();                       // motors idle
  Serial.println(F("# pin-probe ready. Motors idle. Cmd: T pwmPin dirPin dirVal pwmVal ms [stbyPin] | S=stop"));
}

void emergencyStop() {
  allLow();
  Serial.println(F("STOP ok=1 (all pins LOW)"));
}

void doPulse(int pwmPin, int dirPin, int dirVal, int pwmVal, long ms, int stbyPin) {
  if (pwmVal < 0) pwmVal = 0;
  if (pwmVal > PWM_MAX) pwmVal = PWM_MAX;     // clamp power
  if (ms < 0) ms = 0;
  if (ms > PULSE_MAX_MS) ms = PULSE_MAX_MS;   // clamp time

  allLow();
  pinMode(pwmPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  if (stbyPin >= 0) { pinMode(stbyPin, OUTPUT); digitalWrite(stbyPin, HIGH); } // enable driver

  long before = encLeft;
  digitalWrite(dirPin, dirVal ? HIGH : LOW);
  analogWrite(pwmPin, pwmVal);
  digitalWrite(PIN_LED, HIGH);

  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)ms) { /* spin; watchdog via clamp */ }

  analogWrite(pwmPin, 0);
  allLow();                                   // force everything idle again
  digitalWrite(PIN_LED, LOW);
  long delta = encLeft - before;

  Serial.print(F("RESULT pwmPin=")); Serial.print(pwmPin);
  Serial.print(F(" dirPin="));       Serial.print(dirPin);
  Serial.print(F(" dirVal="));       Serial.print(dirVal);
  Serial.print(F(" pwmVal="));       Serial.print(pwmVal);
  Serial.print(F(" ms="));           Serial.print(ms);
  Serial.print(F(" stbyPin="));      Serial.print(stbyPin);
  Serial.print(F(" leftEncDelta=")); Serial.println(delta);
}

// Pulse BOTH motors together on the CONFIRMED pins:
//   STBY=8, LEFT pwm=5/dir=7, RIGHT pwm=6/dir=12.
// Lets us see if the two wheels roll the same way (straight) or opposite (spin).
void doBoth(int leftDir, int rightDir, int pwmVal, long ms) {
  if (pwmVal < 0) pwmVal = 0;
  if (pwmVal > PWM_MAX) pwmVal = PWM_MAX;
  if (ms < 0) ms = 0;
  if (ms > PULSE_MAX_MS) ms = PULSE_MAX_MS;

  allLow();
  pinMode(8, OUTPUT);  digitalWrite(8, HIGH);   // STBY enable
  pinMode(7, OUTPUT);  digitalWrite(7, leftDir  ? HIGH : LOW);
  pinMode(12, OUTPUT); digitalWrite(12, rightDir ? HIGH : LOW);
  pinMode(5, OUTPUT);  pinMode(6, OUTPUT);

  long before = encLeft;
  digitalWrite(PIN_LED, HIGH);
  analogWrite(5, pwmVal);
  analogWrite(6, pwmVal);

  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)ms) { /* clamp watchdog */ }

  analogWrite(5, 0);
  analogWrite(6, 0);
  allLow();
  digitalWrite(PIN_LED, LOW);

  Serial.print(F("BOTH leftDir="));  Serial.print(leftDir);
  Serial.print(F(" rightDir="));     Serial.print(rightDir);
  Serial.print(F(" pwmVal="));       Serial.print(pwmVal);
  Serial.print(F(" ms="));           Serial.print(ms);
  Serial.print(F(" leftEncDelta=")); Serial.println(encLeft - before);
}

void loop() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  char c = line.charAt(0);
  if (c == 'S' || c == 's') { emergencyStop(); return; }

  if (c == 'B' || c == 'b') {
    // Parse: B leftDir rightDir pwmVal ms  -> pulse BOTH motors together.
    int vals[4]; int n = 0; int idx = 1;
    while (n < 4 && idx < (int)line.length()) {
      while (idx < (int)line.length() && line.charAt(idx) == ' ') idx++;
      if (idx >= (int)line.length()) break;
      long v = 0; bool got = false;
      while (idx < (int)line.length() && isDigit(line.charAt(idx))) { v = v*10 + (line.charAt(idx)-'0'); idx++; got = true; }
      if (got) vals[n++] = (int)v;
    }
    if (n < 4) { Serial.println(F("ERR need: B leftDir rightDir pwmVal ms")); return; }
    doBoth(vals[0], vals[1], vals[2], vals[3]);
    return;
  }

  if (c == 'T' || c == 't') {
    // Parse: T pwmPin dirPin dirVal pwmVal ms [stbyPin]
    int vals[6]; int n = 0;
    int idx = 1;
    while (n < 6 && idx < (int)line.length()) {
      while (idx < (int)line.length() && line.charAt(idx) == ' ') idx++;
      if (idx >= (int)line.length()) break;
      int sign = 1;
      if (line.charAt(idx) == '-') { sign = -1; idx++; }
      long v = 0; bool got = false;
      while (idx < (int)line.length() && isDigit(line.charAt(idx))) { v = v*10 + (line.charAt(idx)-'0'); idx++; got = true; }
      if (got) vals[n++] = (int)(sign * v);
    }
    if (n < 5) { Serial.println(F("ERR need: T pwmPin dirPin dirVal pwmVal ms [stbyPin]")); return; }
    int stbyPin = (n >= 6) ? vals[5] : -1;
    doPulse(vals[0], vals[1], vals[2], vals[3], vals[4], stbyPin);
    return;
  }

  emergencyStop();   // anything unexpected -> safe state
}
