#include <Wire.h>
#include <math.h>

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
const int REM_PIN_1 = 2;   // D2 / GPIO2
const int REM_PIN_2 = 15;  // D15 / GPIO15
const unsigned long REM_PULSE_MS = 2000;

unsigned long lastSampleTime = 0;
unsigned long lastEpochTime = 0;

float epochMotionScore = 0.0;
int continuousStillEpochs = 0; // Count consecutive minutes of stillness
int minutesSinceSleep = 0;     // Count minutes since sleep began
bool isSleeping = false;

// ---------------------------------------------------------
// Read the MPU6500 and calculate magnitude (G-force)
// ---------------------------------------------------------
float getAccelMagnitude() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); // Accelerometer starting register
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);
  
  // Read the X, Y, and Z axes (16-bit)
  int16_t ax = Wire.read() << 8 | Wire.read();
  int16_t ay = Wire.read() << 8 | Wire.read();
  int16_t az = Wire.read() << 8 | Wire.read();
  
  // Convert to G units (assuming a +/-2G range; scale is 16384)
  float x = ax / 16384.0;
  float y = ay / 16384.0;
  float z = az / 16384.0;
  
  // Calculate vector magnitude
  float magnitude = sqrt((x * x) + (y * y) + (z * z));
  return magnitude;
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

  pinMode(REM_PIN_1, OUTPUT);
  pinMode(REM_PIN_2, OUTPUT);
  digitalWrite(REM_PIN_1, LOW);
  digitalWrite(REM_PIN_2, LOW);
  
  // Wake the MPU6500 from sleep mode
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); // Power Management 1 register
  Wire.write(0);    // set to zero (wakes up the MPU)
  Wire.endTransmission(true);
  
  Serial.println("System Ready. Starting Sleep Tracker...");
}

// ---------------------------------------------------------
// Main Loop
// ---------------------------------------------------------
void loop() {
  unsigned long currentTime = millis();
  
  // 1. Acquire data every 100 ms
  if (currentTime - lastSampleTime >= SAMPLE_RATE_MS) {
    lastSampleTime = currentTime;
    
    float mag = getAccelMagnitude();
    // Subtract Earth's gravity (1G) and take the absolute value
    float motion = abs(mag - 1.0); 
    
    // Accumulate motion over one epoch
    epochMotionScore += motion; 
  }
  
  // 2. Process every minute (epoch windowing)
  if (currentTime - lastEpochTime >= EPOCH_DURATION_MS) {
    lastEpochTime = currentTime;
    
    // Print the raw value to the Serial Monitor
    Serial.print("Epoch Motion Score: ");
    Serial.print(epochMotionScore);
    
    // Update the stillness counter
    if (epochMotionScore < T_STILL) {
      continuousStillEpochs++;
    } else {
      continuousStillEpochs = 0; // Movement resets the counter
    }
    
    // ----- State machine for sleep detection -----
    if (!isSleeping) {
      // Sleep onset condition: still for 15 consecutive minutes
      if (continuousStillEpochs >= 15) {
        isSleeping = true;
        minutesSinceSleep = 15; // Include the 15 minutes that have already passed
        currentState = STATE_SLEEPING_NREM;
        Serial.println(" --> EVENT: Sleep Onset Detected!");
      } else {
        currentState = STATE_AWAKE;
      }
    } 
    else {
      // If sleeping, increment the elapsed time
      minutesSinceSleep++;
      
      // Significant movement (awake or changing sleeping position)
      if (epochMotionScore > T_MOVE) {
        currentState = STATE_AWAKE;
        // Reset sleep tracking after being awake for more than 5 minutes (adjust as needed)
        if (continuousStillEpochs == 0) { 
           // Treat this as light sleep or restless sleep for now.
           // Do not reset isSleeping until movement continues for a longer period.
           currentState = STATE_SLEEPING_NREM; // Or change to LIGHT_SLEEP
        }
      } 
      // Body remains still
      else if (epochMotionScore < T_STILL) {
        // Check whether this is a REM window and whether the body has been still long enough (e.g. 5 minutes)
        if (isInREMWindow(minutesSinceSleep) && continuousStillEpochs >= 5) {
          currentState = STATE_POSSIBLE_REM;
        } else {
          currentState = STATE_SLEEPING_NREM; // Deep/light sleep
        }
      }
    }
    
    // When entering POSSIBLE REM, set D2 and D15 HIGH for 2 seconds.
    if (currentState == STATE_POSSIBLE_REM && previousState != STATE_POSSIBLE_REM) {
      digitalWrite(REM_PIN_1, HIGH);
      digitalWrite(REM_PIN_2, HIGH);
      delay(REM_PULSE_MS);
      digitalWrite(REM_PIN_1, LOW);
      digitalWrite(REM_PIN_2, LOW);
    }
    previousState = currentState;

    // Display the current state
    Serial.print(" | Minutes: ");
    Serial.print(minutesSinceSleep);
    Serial.print(" | State: ");
    if (currentState == STATE_AWAKE) Serial.println("AWAKE");
    else if (currentState == STATE_SLEEPING_NREM) Serial.println("NREM (Deep/Light)");
    else if (currentState == STATE_POSSIBLE_REM) Serial.println("POSSIBLE REM");
    
    // Reset accumulated values for the next epoch
    epochMotionScore = 0;
  }
}