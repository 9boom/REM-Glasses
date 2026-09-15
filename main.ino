#include <Wire.h>
#include <math.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ---------------------------------------------------------
// Constants and settings
// ---------------------------------------------------------
const int MPU_ADDR = 0x68; // I2C address of the MPU6500/MPU6050

// Time settings (milliseconds)
const unsigned long SAMPLE_RATE_MS = 100;     // Read every 100 ms (10 Hz)
const unsigned long EPOCH_DURATION_MS = 60000;// 1 epoch = 60 seconds (1 minute)

// Threshold settings (calibrate with the actual sensor)
const float T_STILL = 15.0;  // Accumulated motion considered "still"
const float T_MOVE = 50.0;   // Accumulated motion considered "moving/awake"

// ---------------------------------------------------------
// System variables
// ---------------------------------------------------------
enum SleepState {
  STATE_AWAKE,
  STATE_SLEEPING_NREM,
  STATE_POSSIBLE_REM
};

SleepState currentState = STATE_AWAKE;
SleepState previousState = STATE_AWAKE;

// ESP32 GPIO pins
const int REM_PIN_1 = 4;   // D2 / GPIO2
const int REM_PIN_2 = 15;  // D15 / GPIO15
const unsigned long REM_PULSE_MS = 2000;

// Serial debug modes
bool verboseMode = false;
bool debugMode = false;

// MPU6500 calibration
bool calibrationValid = false;
float accelOffsetX = 0.0f;
float accelOffsetY = 0.0f;
float accelOffsetZ = 0.0f;
float gyroOffsetX = 0.0f;
float gyroOffsetY = 0.0f;
float gyroOffsetZ = 0.0f;
const int CALIBRATION_SAMPLES = 2000;
const unsigned long CALIBRATION_SAMPLE_INTERVAL_MS = 5;
const float CALIBRATION_STILL_STD_G = 0.035f;
const unsigned long CALIBRATION_BLINK_INTERVAL_MS = 500; // 0.5 s ON / 0.5 s OFF
const unsigned long CALIBRATION_RETRY_PULSE_MS = 500;    // D2 HIGH for 0.5 s on failure

// DEBUG mode: use shorter values for real-world dry-run testing
const unsigned long DEBUG_SAMPLE_RATE_MS = 100;
const unsigned long DEBUG_EPOCH_DURATION_MS = 5000;
const float DEBUG_T_STILL = 2.0;
const float DEBUG_T_MOVE = 5.0;
const int DEBUG_SLEEP_ONSET_EPOCHS = 3;
const int DEBUG_REM_STILL_EPOCHS = 2;

// TEST pulse control
bool testPulseActive = false;
unsigned long testPulseStartTime = 0;

unsigned long lastSampleTime = 0;
unsigned long lastEpochTime = 0;

float epochMotionScore = 0.0;
int continuousStillEpochs = 0; // Count consecutive minutes of stillness
int minutesSinceSleep = 0;     // Count minutes since sleep began
bool isSleeping = false;

// ---------------------------------------------------------
// Helper functions
// ---------------------------------------------------------
const char* stateName(SleepState state) {
  if (state == STATE_AWAKE) return "AWAKE";
  if (state == STATE_SLEEPING_NREM) return "NREM (Deep/Light)";
  return "POSSIBLE REM";
}

bool serialReadCommand(String &command) {
  if (!Serial.available()) return false;

  command = Serial.readStringUntil('\n');
  command.trim();
  return command.length() > 0;
}

void startRemPulse(const char* reason) {
  digitalWrite(REM_PIN_1, HIGH);
  digitalWrite(REM_PIN_2, HIGH);
  testPulseActive = true;
  testPulseStartTime = millis();

  if (verboseMode) {
    Serial.print("[VERBOSE] D2/D15 HIGH -> ");
    Serial.println(reason);
  }
}

void updateRemPulse() {
  if (testPulseActive && millis() - testPulseStartTime >= REM_PULSE_MS) {
    digitalWrite(REM_PIN_1, LOW);
    digitalWrite(REM_PIN_2, LOW);
    testPulseActive = false;

    if (verboseMode) {
      Serial.println("[VERBOSE] D2/D15 LOW -> pulse complete");
    }
  }
}

void printVerboseHeader() {
  Serial.println("[VERBOSE] ------------------------------------------------");
  Serial.println("[VERBOSE] Live debug enabled");
  Serial.print("[VERBOSE] Mode: ");
  Serial.println(debugMode ? "DEBUG" : "NORMAL");
  Serial.print("[VERBOSE] Calibration: ");
  Serial.println(calibrationValid ? "VALID" : "NOT CALIBRATED");
  Serial.print("[VERBOSE] MPU6500 I2C SDA=GPIO21, SCL=GPIO22, ADDR=0x");
  Serial.println(MPU_ADDR, HEX);
  Serial.print("[VERBOSE] SAMPLE_RATE_MS=");
  Serial.println(debugMode ? DEBUG_SAMPLE_RATE_MS : SAMPLE_RATE_MS);
  Serial.print("[VERBOSE] EPOCH_DURATION_MS=");
  Serial.println(debugMode ? DEBUG_EPOCH_DURATION_MS : EPOCH_DURATION_MS);
  Serial.println("[VERBOSE] ------------------------------------------------");
}

// ---------------------------------------------------------
// MPU6500 low-level read + calibration
// ---------------------------------------------------------
// Raw MPU6500 values are stored in globals to avoid Arduino IDE
// auto-prototype issues with custom struct types in function signatures.
int16_t rawAx = 0;
int16_t rawAy = 0;
int16_t rawAz = 0;
int16_t rawGx = 0;
int16_t rawGy = 0;
int16_t rawGz = 0;

bool readSensorRaw() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;

  if (Wire.requestFrom(MPU_ADDR, 14, true) != 14) return false;

  rawAx = (int16_t)((Wire.read() << 8) | Wire.read());
  rawAy = (int16_t)((Wire.read() << 8) | Wire.read());
  rawAz = (int16_t)((Wire.read() << 8) | Wire.read());
  (void)Wire.read(); (void)Wire.read(); // temperature
  rawGx = (int16_t)((Wire.read() << 8) | Wire.read());
  rawGy = (int16_t)((Wire.read() << 8) | Wire.read());
  rawGz = (int16_t)((Wire.read() << 8) | Wire.read());
  return true;
}

void printCalibration() {
  Serial.println("[CAL] ------------------------------");
  Serial.print("[CAL] Status: ");
  Serial.println(calibrationValid ? "VALID" : "NOT CALIBRATED");
  Serial.print("[CAL] Accel offset (g): X=");
  Serial.print(accelOffsetX, 6);
  Serial.print(" Y=");
  Serial.print(accelOffsetY, 6);
  Serial.print(" Z=");
  Serial.println(accelOffsetZ, 6);
  Serial.print("[CAL] Gyro offset (dps): X=");
  Serial.print(gyroOffsetX, 6);
  Serial.print(" Y=");
  Serial.print(gyroOffsetY, 6);
  Serial.print(" Z=");
  Serial.println(gyroOffsetZ, 6);
  Serial.println("[CAL] ------------------------------");
}

void setCalibrationIndicator(bool on) {
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
  digitalWrite(REM_PIN_2, on ? HIGH : LOW);
}

void failCalibrationPulse() {
  digitalWrite(REM_PIN_1, HIGH);
  const unsigned long start = millis();
  while (millis() - start < CALIBRATION_RETRY_PULSE_MS) {
    delay(1);
  }
  digitalWrite(REM_PIN_1, LOW);
}

void calibrationWaitWithBlink(unsigned long durationMs, unsigned long &blinkTimer, bool &blinkState) {
  const unsigned long start = millis();
  while (millis() - start < durationMs) {
    const unsigned long now = millis();
    if (now - blinkTimer >= CALIBRATION_BLINK_INTERVAL_MS) {
      blinkTimer = now;
      blinkState = !blinkState;
      setCalibrationIndicator(blinkState);
    }
    delay(1);
  }
}

bool runCalibration() {
  Serial.println("\n=== MPU6500 CALIBRATION ===");
  Serial.println("Place the sensor in the same fixed orientation used during operation.");
  Serial.println("Keep it completely still.");
  Serial.println("Builtin LED + D15 blink every 0.5 s while calibration is running.");

  digitalWrite(REM_PIN_1, LOW);
  setCalibrationIndicator(false);
  unsigned long blinkTimer = millis();
  bool blinkState = false;

  Serial.println("Starting in 3...");
  calibrationWaitWithBlink(1000, blinkTimer, blinkState);
  Serial.println("2...");
  calibrationWaitWithBlink(1000, blinkTimer, blinkState);
  Serial.println("1...");
  calibrationWaitWithBlink(1000, blinkTimer, blinkState);

  double sumAx = 0, sumAy = 0, sumAz = 0;
  double sumGx = 0, sumGy = 0, sumGz = 0;
  double sumMag = 0, sumMag2 = 0;
  int validSamples = 0;
  unsigned long nextSample = millis();
  int lastPercent = -1;

  for (int i = 0; i < CALIBRATION_SAMPLES; ) {
    const unsigned long now = millis();

    if (now - blinkTimer >= CALIBRATION_BLINK_INTERVAL_MS) {
      blinkTimer = now;
      blinkState = !blinkState;
      setCalibrationIndicator(blinkState);
    }

    if (now < nextSample) {
      delay(1);
      continue;
    }
    nextSample += CALIBRATION_SAMPLE_INTERVAL_MS;

      if (!readSensorRaw()) {
      Serial.println("[CAL] Sensor read error. Calibration FAILED.");
      setCalibrationIndicator(false);
      return false;
    }

    const float ax = rawAx / 16384.0f;
    const float ay = rawAy / 16384.0f;
    const float az = rawAz / 16384.0f;
    const float mag = sqrtf(ax * ax + ay * ay + az * az);

    sumAx += ax; sumAy += ay; sumAz += az;
    sumGx += rawGx / 131.0;
    sumGy += rawGy / 131.0;
    sumGz += rawGz / 131.0;
    sumMag += mag;
    sumMag2 += (double)mag * mag;
    validSamples++;

    const int percent = (validSamples * 100) / CALIBRATION_SAMPLES;
    if (percent / 5 != lastPercent / 5) {
      lastPercent = percent;
      Serial.print("[CAL] Progress: ");
      Serial.print(percent);
      Serial.println("%");
    }
    i++;
  }

  const float meanAx = sumAx / validSamples;
  const float meanAy = sumAy / validSamples;
  const float meanAz = sumAz / validSamples;
  const float meanMag = sumMag / validSamples;
  const float variance = (sumMag2 / validSamples) - (double)meanMag * meanMag;
  const float stdMag = sqrtf(max(0.0f, (float)variance));

  if (fabsf(meanMag - 1.0f) > 0.12f || stdMag > CALIBRATION_STILL_STD_G) {
    Serial.println("[CAL] Calibration FAILED: sensor was not stable or gravity reading is invalid.");
    Serial.print("[CAL] Mean magnitude = ");
    Serial.print(meanMag, 5);
    Serial.print(" G, StdDev = ");
    Serial.print(stdMag, 5);
    Serial.println(" G");
    setCalibrationIndicator(false);
    return false;
  }

  const float absX = fabsf(meanAx);
  const float absY = fabsf(meanAy);
  const float absZ = fabsf(meanAz);

  float expectedAx = 0.0f;
  float expectedAy = 0.0f;
  float expectedAz = 0.0f;
  if (absX >= absY && absX >= absZ) expectedAx = (meanAx >= 0.0f) ? 1.0f : -1.0f;
  else if (absY >= absX && absY >= absZ) expectedAy = (meanAy >= 0.0f) ? 1.0f : -1.0f;
  else expectedAz = (meanAz >= 0.0f) ? 1.0f : -1.0f;

  accelOffsetX = meanAx - expectedAx;
  accelOffsetY = meanAy - expectedAy;
  accelOffsetZ = meanAz - expectedAz;
  gyroOffsetX = (float)(sumGx / validSamples);
  gyroOffsetY = (float)(sumGy / validSamples);
  gyroOffsetZ = (float)(sumGz / validSamples);
  calibrationValid = true;

  setCalibrationIndicator(false);
  Serial.println("[CAL] Calibration successful.");
  printCalibration();
  Serial.println("[CAL] Returning to normal operation.");
  return true;
}

// Read the MPU6500 and calculate calibrated magnitude (G-force)
float getAccelMagnitude() {
  if (!readSensorRaw()) return NAN;

  float x = (rawAx / 16384.0f) - accelOffsetX;
  float y = (rawAy / 16384.0f) - accelOffsetY;
  float z = (rawAz / 16384.0f) - accelOffsetZ;

  return sqrtf((x * x) + (y * y) + (z * z));
}

// ---------------------------------------------------------
// Check the REM time window (based on sleep architecture)
// ---------------------------------------------------------
bool isInREMWindow(int minutes) {
  // Cycle 1: ~90 minutes (10-15 minute duration)
  if (minutes >= 80 && minutes <= 100) return true;
  // Cycle 2: ~180 minutes
  if (minutes >= 170 && minutes <= 200) return true;
  // Cycle 3: ~270 minutes
  if (minutes >= 255 && minutes <= 295) return true;
  // Cycle 4+: REM periods become longer in the second half of the night
  if (minutes >= 340) return true; 
  
  return false;
}

// ---------------------------------------------------------
// Setup
// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // MPU6500: SDA = GPIO21 (D21), SCL = GPIO22 (D22)
  Wire.begin(21, 22);

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(REM_PIN_1, OUTPUT);
  pinMode(REM_PIN_2, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(REM_PIN_1, LOW);
  digitalWrite(REM_PIN_2, LOW);

  // Wake the MPU6500 from sleep mode
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); // Power Management 1 register
  Wire.write(0);    // set to zero (wakes up the MPU)
  Wire.endTransmission(true);

  // Every boot must calibrate successfully before the main logic starts.
  while (!runCalibration()) {
    Serial.println("[CAL] Failed. D2 will pulse HIGH for 0.5 s, then calibration will retry.");
    failCalibrationPulse();
  }

  epochMotionScore = 0.0f;
  continuousStillEpochs = 0;
  minutesSinceSleep = 0;
  isSleeping = false;
  currentState = STATE_AWAKE;
  previousState = STATE_AWAKE;
  lastSampleTime = millis();
  lastEpochTime = millis();

  Serial.println("System Ready. Starting Sleep Tracker...");
}

// ---------------------------------------------------------
// Main Loop
// ---------------------------------------------------------
void loop() {
  String command;

  // 0. Handle commands from Serial Monitor
  if (serialReadCommand(command)) {
    if (command == "!VERBOSE") {
      verboseMode = !verboseMode;
      Serial.print("VERBOSE mode: ");
      Serial.println(verboseMode ? "ON" : "OFF");
      if (verboseMode) printVerboseHeader();
    }
    else if (command == "!TEST") {
      startRemPulse("!TEST");
      Serial.println("TEST: D2 and D15 HIGH for 2 seconds");
    }
    else if (command == "!CALIBRATE") {
      // Avoid a calibration run while the output pulse is active.
      if (testPulseActive) {
        digitalWrite(REM_PIN_1, LOW);
        digitalWrite(REM_PIN_2, LOW);
        testPulseActive = false;
      }
      bool success = false;
      while (!success) {
        success = runCalibration();
        if (!success) {
          Serial.println("[CAL] Failed. D2 will pulse HIGH for 0.5 s, then calibration will retry.");
          failCalibrationPulse();
        }
      }

      // Reset timing accumulators so calibration data does not leak into the next epoch.
      epochMotionScore = 0.0f;
      continuousStillEpochs = 0;
      minutesSinceSleep = 0;
      isSleeping = false;
      currentState = STATE_AWAKE;
      previousState = STATE_AWAKE;
      lastSampleTime = millis();
      lastEpochTime = millis();
      Serial.println("CALIBRATE: SUCCESS");
    }
    else if (command == "!DEBUG") {
      debugMode = !debugMode;
      continuousStillEpochs = 0;
      minutesSinceSleep = 0;
      isSleeping = false;
      currentState = STATE_AWAKE;
      previousState = STATE_AWAKE;
      epochMotionScore = 0.0;
      lastSampleTime = millis();
      lastEpochTime = millis();
      Serial.print("DEBUG mode: ");
      Serial.println(debugMode ? "ON" : "OFF");
      if (debugMode) {
        Serial.println("DEBUG: shortened timing/thresholds enabled for real-use dry run");
      }
    }
    else {
      Serial.print("Unknown command: ");
      Serial.println(command);
      Serial.println("Commands: !VERBOSE  !TEST  !DEBUG  !CALIBRATE");
    }
  }

  updateRemPulse();

  unsigned long currentTime = millis();
  const unsigned long activeSampleRate = debugMode ? DEBUG_SAMPLE_RATE_MS : SAMPLE_RATE_MS;
  const unsigned long activeEpochDuration = debugMode ? DEBUG_EPOCH_DURATION_MS : EPOCH_DURATION_MS;
  const float activeTStill = debugMode ? DEBUG_T_STILL : T_STILL;
  const float activeTMove = debugMode ? DEBUG_T_MOVE : T_MOVE;
  const int activeSleepOnsetEpochs = debugMode ? DEBUG_SLEEP_ONSET_EPOCHS : 15;
  const int activeRemStillEpochs = debugMode ? DEBUG_REM_STILL_EPOCHS : 5;

  // 1. Acquire data at the active sample rate
  if (currentTime - lastSampleTime >= activeSampleRate) {
    lastSampleTime = currentTime;

    float mag = getAccelMagnitude();
    float motion = fabs(mag - 1.0);

    if (isnan(mag)) {
      if (verboseMode) Serial.println("[VERBOSE] MPU6500 read ERROR");
    }
    else {
      epochMotionScore += motion;

      if (verboseMode) {
        Serial.print("[VERBOSE] MPU | Mag=");
        Serial.print(mag, 4);
        Serial.print(" G | Motion=");
        Serial.print(motion, 4);
        Serial.print(" | EpochScore=");
        Serial.print(epochMotionScore, 4);
        Serial.print(" | Cal=");
        Serial.print(calibrationValid ? "OK" : "NONE");
        Serial.println();
      }
    }
  }

  // 2. Process the active epoch
  if (currentTime - lastEpochTime >= activeEpochDuration) {
    lastEpochTime = currentTime;

    if (verboseMode) {
      Serial.println("[VERBOSE] ===== EPOCH =====");
      Serial.print("[VERBOSE] MotionScore=");
      Serial.println(epochMotionScore, 4);
      Serial.print("[VERBOSE] Still epochs=");
      Serial.println(continuousStillEpochs);
    }

    if (epochMotionScore < activeTStill) {
      continuousStillEpochs++;
    } else {
      continuousStillEpochs = 0;
    }

    // ----- State machine for sleep detection -----
    if (!isSleeping) {
      if (continuousStillEpochs >= activeSleepOnsetEpochs) {
        isSleeping = true;
        minutesSinceSleep = debugMode ? 1 : 15;
        currentState = STATE_SLEEPING_NREM;
        Serial.println(" --> EVENT: Sleep Onset Detected!");
      } else {
        currentState = STATE_AWAKE;
      }
    }
    else {
      minutesSinceSleep++;

      if (epochMotionScore > activeTMove) {
        currentState = STATE_AWAKE;
        if (continuousStillEpochs == 0) {
          currentState = STATE_SLEEPING_NREM;
        }
      }
      else if (epochMotionScore < activeTStill) {
        // In DEBUG mode use compressed REM windows so the behavior can be exercised quickly.
        bool remWindow = debugMode
          ? (minutesSinceSleep >= 2 && minutesSinceSleep <= 6)
          : isInREMWindow(minutesSinceSleep);

        if (remWindow && continuousStillEpochs >= activeRemStillEpochs) {
          currentState = STATE_POSSIBLE_REM;
        } else {
          currentState = STATE_SLEEPING_NREM;
        }
      }
    }

    // Entering POSSIBLE REM -> D2/D15 HIGH for 2 seconds.
    if (currentState == STATE_POSSIBLE_REM && previousState != STATE_POSSIBLE_REM) {
      startRemPulse("STATE_POSSIBLE_REM");
    }
    previousState = currentState;

    Serial.print("Epoch Motion Score: ");
    Serial.print(epochMotionScore, 4);
    Serial.print(" | Minutes: ");
    Serial.print(minutesSinceSleep);
    Serial.print(" | State: ");
    Serial.println(stateName(currentState));

    if (verboseMode) {
      Serial.print("[VERBOSE] thresholds: STILL=");
      Serial.print(activeTStill);
      Serial.print(" MOVE=");
      Serial.print(activeTMove);
      Serial.print(" | sleepOnset=");
      Serial.print(activeSleepOnsetEpochs);
      Serial.print(" epochs | REMStill=");
      Serial.println(activeRemStillEpochs);
    }

    epochMotionScore = 0;
  }
}
