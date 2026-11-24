/******
**.  TINYPenPlotter V2
*/
#pragma once
#define MOTOR_TYPE_NEMA17 0
#define MOTOR_TYPE_28BYJ48 1

#define MOTOR_TYPE MOTOR_TYPE_NEMA17

// Select the motor type used by this build: UNCOMMENT the one you want, only 1 can be used 
//#define MOTOR_TYPE MOTOR_TYPE_28BYJ48
#define MOTOR_TYPE MOTOR_TYPE_NEMA17


//End stops
constexpr int X_MIN_PIN = 0;
constexpr int Y_MIN_PIN = 1;
//SErvo pin
constexpr int SERVO_PIN = 10;

/*****
Spoecifics for 28BYJ48
*/
#if MOTOR_TYPE == MOTOR_TYPE_28BYJ48
// These are ULN2003 IN1–IN4 pins, not STEP/DIR pins.
constexpr int X_IN1_PIN = 4;
constexpr int X_IN2_PIN = 5;
constexpr int X_IN3_PIN = 6;
constexpr int X_IN4_PIN = 7;

constexpr int Y_IN1_PIN = 8;
constexpr int Y_IN2_PIN = 9;
constexpr int Y_IN3_PIN = 20;
constexpr int Y_IN4_PIN = 21;

constexpr int8_t X_MOTOR_DIRECTION = 1;
constexpr int8_t Y_MOTOR_DIRECTION = -1;

//Steps
constexpr float X_STEPS_PER_MM = 52.8;//300.95f;
constexpr float Y_STEPS_PER_MM = 52.8;//300.95f;

constexpr long BEDSIZE_X = 90;  // mm
constexpr long BEDSIZE_Y = 100;  // mm

//Assume 28BYJ48, 20 tooth gear, GT2 belt.
// Conservative starting values for testing, not final performance limits.
constexpr float MAX_FEED_MM_MIN = 2000.0f;
constexpr float HOMING_FEED_MM_MIN = 300.0f;
constexpr float MAX_ACCELERATION_MM_S2 = 200.0f;  // steps/s^2
constexpr long HOMING_MAX_SEEK_STEPS_X = lroundf(BEDSIZE_X * X_STEPS_PER_MM);
constexpr long HOMING_MAX_SEEK_STEPS_Y = lroundf(BEDSIZE_Y * Y_STEPS_PER_MM);
constexpr long HOMING_CHUNK_SIZE = 2*X_STEPS_PER_MM;  // steps; about 2 mm at 51 steps/mm
#else
constexpr int X_STEP_PIN = 4;
constexpr int X_DIR_PIN = 5;
constexpr int Y_STEP_PIN = 6;
constexpr int Y_DIR_PIN = 7;

//DIRECTION
constexpr int8_t X_MOTOR_DIRECTION = 1;
constexpr int8_t Y_MOTOR_DIRECTION = 1;

// Assumes GT2 (2 mm pitch), 20-tooth pulleys, 1.8° motors, A4988 at 1/16.
constexpr float X_STEPS_PER_MM = 80.0f;
constexpr float Y_STEPS_PER_MM = 80.0f;
constexpr long BEDSIZE_X = 240;
constexpr long BEDSIZE_Y = 320;
constexpr float MAX_FEED_MM_MIN = 2500.0f;
constexpr float MAX_ACCELERATION_MM_S2 = 200.0f;  // steps/s^2
constexpr float HOMING_FEED_MM_MIN = 1000.0f;              //not to fast homing speed.  it could slam into the end stop.
constexpr float HOMING_CHUNK_SIZE = 10 * X_STEPS_PER_MM;            //1cm
constexpr long HOMING_MAX_SEEK_STEPS_X = lroundf(BEDSIZE_X * X_STEPS_PER_MM);
constexpr long HOMING_MAX_SEEK_STEPS_Y = lroundf(BEDSIZE_Y * Y_STEPS_PER_MM);
#endif


// Defaults used until overridden by saved web settings.
// With INPUT_PULLUP, pressed-to-GND switches are active-low.
constexpr bool X_ENDSTOP_ACTIVE_LOW_DEFAULT = true;
constexpr bool Y_ENDSTOP_ACTIVE_LOW_DEFAULT = true;


constexpr float HOME_BACKOFF_MM = 3.0f;
constexpr float DEFAULT_DRAWING_FEED_MM_MIN = 4000.0f;

// Calibrate these for your pen linkage and servo.
constexpr int PEN_UP_US = 1700;
constexpr int PEN_DOWN_US = 1100;

constexpr char WIFI_SSID[] = "FergieBot-pen-plotter";
constexpr char WIFI_PASSWORD[] = "ferg1234";

// Prevent USB and web commands from controlling the plotter simultaneously.
constexpr bool CONTROL_SOURCE_LOCKING_ENABLED = true;
