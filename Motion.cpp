/******
**.  TINYPenPlotter V2
*/
#include "Motion.h"
#include "Config.h"
#include <Preferences.h>
#include <AccelStepper.h>
#include <ESP32Servo.h>
#include <math.h>
#include "Debugger.h"

#if MOTOR_TYPE == MOTOR_TYPE_28BYJ48
static AccelStepper xMotor(
  AccelStepper::FULL4WIRE,
  X_IN1_PIN, X_IN3_PIN, X_IN2_PIN, X_IN4_PIN);

static AccelStepper yMotor(  AccelStepper::FULL4WIRE,
  Y_IN1_PIN, Y_IN3_PIN, Y_IN2_PIN, Y_IN4_PIN);
#else
static AccelStepper xMotor(
  AccelStepper::DRIVER, X_STEP_PIN, X_DIR_PIN);

static AccelStepper yMotor(
  AccelStepper::DRIVER, Y_STEP_PIN, Y_DIR_PIN);
#endif
static Servo penServo;
uint32_t moveStartedMs;


void Motion::begin() {
  pinMode(X_MIN_PIN, INPUT_PULLUP);
  pinMode(Y_MIN_PIN, INPUT_PULLUP);
  Preferences preferences;
  if (preferences.begin("fergoplotter", true)) {
    const float savedXRate =
      preferences.getFloat("x_rate", MAX_FEED_MM_MIN);
    const float savedYRate =
      preferences.getFloat("y_rate", MAX_FEED_MM_MIN);
    const float savedHomeRate =
      preferences.getFloat("home_rate", homingSeekRateMmMin_);

    const float savedX = preferences.getFloat("x_steps", X_STEPS_PER_MM);
    const float savedY = preferences.getFloat("y_steps", Y_STEPS_PER_MM);
    const bool savedXEndstop = preferences.getBool("x_endstop_low", X_ENDSTOP_ACTIVE_LOW_DEFAULT);
    const bool savedYEndstop = preferences.getBool("y_endstop_low", Y_ENDSTOP_ACTIVE_LOW_DEFAULT);

    preferences.end();
    xEndstopActiveLow_ = savedXEndstop;
    yEndstopActiveLow_ = savedYEndstop;

    if (isfinite(savedX) && savedX >= 1.0f && savedX <= 2000.0f) {
      xStepsPerMm_ = savedX;
    }
    if (isfinite(savedY) && savedY >= 1.0f && savedY <= 2000.0f) {
      yStepsPerMm_ = savedY;
    }
    xMotor.setCurrentPosition(0);
    yMotor.setCurrentPosition(0);
  }


  //xMotor.setMaxSpeed(3000);
  setMaxSpeed(xMotor, "X", MAX_FEED_MM_MIN);
  setMaxSpeed(yMotor, "Y", MAX_FEED_MM_MIN);


  penServo.setPeriodHertz(50);
  penServo.attach(SERVO_PIN, 500, 2500);
  pen(true);  // Pen up
}

void Motion::setMaxSpeed(AccelStepper& motor, String axis, int max) {
  Debugger::log(String("Setting maxSpeed for motor ") + axis + " to " + String(max));
  motor.setMaxSpeed(max);
}


void Motion::service() {
  if (!busy_) {
    PlannedMove move;
    if (popPlannedMove(move)) {
      moveTo(move.xMm, move.yMm, move.feedMmMin);
    }
    return;
  }

  xMotor.run();
  yMotor.run();

  if (xMotor.distanceToGo() == 0 && yMotor.distanceToGo() == 0) {
    busy_ = false;
    xMm_ = xMotor.currentPosition() / xStepsPerMm_;
    yMm_ = yMotor.currentPosition() / yStepsPerMm_;
    // Keep your existing completion logging here.
    const uint32_t elapsedMs = millis() - moveStartedMs;

    Debugger::log(String("[") + String(millis()) + " ms] Motion complete: elapsed=" + String(elapsedMs) + " ms" + " X steps=" + String(xMotor.currentPosition()) + " Y steps=" + String(yMotor.currentPosition()));
  }
}

bool Motion::moveTo(float xMm, float yMm, float feedMmMin) {
  if (busy_) {
    Debugger::log("moveTo rejected: motion is already busy");
    return false;
  }

  if (!isfinite(xMm) || !isfinite(yMm)) {
    Debugger::log("moveTo rejected: X or Y position is not finite");
    return false;
  }

  if (!isfinite(feedMmMin) || feedMmMin <= 0.0f) {
    Debugger::log(String("moveTo rejected: invalid feed ") + String(feedMmMin));
    return false;
  }

  if (!isfinite(xStepsPerMm_) || xStepsPerMm_ <= 0.0f || !isfinite(yStepsPerMm_) || yStepsPerMm_ <= 0.0f) {
    Debugger::log("moveTo rejected: invalid steps/mm configuration");
    return false;
  }

  if (!isfinite(MAX_ACCELERATION_MM_S2) || MAX_ACCELERATION_MM_S2 <= 0.0f) {
    Debugger::log("moveTo rejected: invalid path acceleration configuration");
    return false;
  }

  moveStartedMs = millis();

  if (feedMmMin > MAX_FEED_MM_MIN) {
    Debugger::log(
      "Feed WAS " + String(feedMmMin) + " but capped at MAX_FEED_MM_MIN=" + String(MAX_FEED_MM_MIN));
    feedMmMin = MAX_FEED_MM_MIN;
  }

  const long xt = lroundf(xMm * xStepsPerMm_);
  const long yt = lroundf(yMm * yStepsPerMm_);

  const double dx =
    (static_cast<double>(xt) - xMotor.currentPosition()) / xStepsPerMm_;
  const double dy =
    (static_cast<double>(yt) - yMotor.currentPosition()) / yStepsPerMm_;
  const double distance = hypot(dx, dy);

  if (!isfinite(distance)) {
    Debugger::log("moveTo rejected: calculated distance is not finite");
    return false;
  }

  if (distance < 0.001) {
    Debugger::log("moveTo accepted as no-op: target is already reached");
    xMm_ = xMm;
    yMm_ = yMm;
    return true;
  }

  const float ux = static_cast<float>(fabs(dx / distance));
  const float uy = static_cast<float>(fabs(dy / distance));

  float limitedFeed = min(feedMmMin, MAX_FEED_MM_MIN);

  if (ux > 0.0f) {
    limitedFeed = min(limitedFeed, xMaxRateMmMin_ / ux);
  }
  if (uy > 0.0f) {
    limitedFeed = min(limitedFeed, yMaxRateMmMin_ / uy);
  }

  if (!isfinite(limitedFeed) || limitedFeed <= 0.0f) {
    Debugger::log("moveTo rejected: effective feed is invalid");
    return false;
  }

  const float feed = limitedFeed / 60.0f;  // mm/s

  const float xSpeed = ux * feed * xStepsPerMm_;  // steps/s
  const float ySpeed = uy * feed * yStepsPerMm_;  // steps/s

  const float pathAcceleration = MAX_ACCELERATION_MM_S2;  // mm/s^2
  const float xAcceleration =
    ux * pathAcceleration * xStepsPerMm_;  // steps/s^2
  const float yAcceleration =
    uy * pathAcceleration * yStepsPerMm_;  // steps/s^2

  Debugger::log(
    String("[") + String(moveStartedMs) + " ms] moveTo start: target X=" + String(xMm, 3) + " Y=" + String(yMm, 3) + " distance=" + String(static_cast<float>(distance), 3) + " mm requested F=" + String(feedMmMin, 1) + " effective F=" + String(limitedFeed, 1) + " mm/min Xspeed=" + String(xSpeed, 1) + " Yspeed=" + String(ySpeed, 1) + " steps/s");

  if (ux > 0.0f) {
    setMaxSpeed(xMotor, "X", xSpeed);
    xMotor.setAcceleration(xAcceleration);
  }

  if (uy > 0.0f) {
    setMaxSpeed(yMotor, "Y", ySpeed);
    yMotor.setAcceleration(yAcceleration);
  }

  xMotor.moveTo(xt);
  yMotor.moveTo(yt);
  busy_ = true;

  Debugger::log("moveTo accepted: motor targets set; busy=true");
  return true;
}
bool Motion::plannerEmpty() const {
  return plannerCount_ == 0;
}

bool Motion::plannerFull() const {
  return plannerCount_ >= PLANNER_CAPACITY;
}

bool Motion::enqueuePlannedMove(float xMm, float yMm, float feedMmMin) {
  if (!isfinite(xMm) || !isfinite(yMm) || !isfinite(feedMmMin) || feedMmMin <= 0.0f) {
    Debugger::log("Planner rejected move: invalid position or feed");
    return false;
  }

  if (plannerFull()) {
    return false;
  }

  planner_[plannerHead_] = { xMm, yMm, feedMmMin };
  plannerHead_ = (plannerHead_ + 1) % PLANNER_CAPACITY;
  ++plannerCount_;
  return true;
}

bool Motion::peekPlannedMove(PlannedMove& move) const {
  if (plannerEmpty()) {
    return false;
  }

  move = planner_[plannerTail_];
  return true;
}

bool Motion::popPlannedMove(PlannedMove& move) {
  if (plannerEmpty()) {
    return false;
  }

  move = planner_[plannerTail_];
  plannerTail_ = (plannerTail_ + 1) % PLANNER_CAPACITY;
  --plannerCount_;
  return true;
}



bool Motion::switchPressed(int pin) {
  const int value = digitalRead(pin);
  const bool activeLow = (pin == X_MIN_PIN)
      ? xEndstopActiveLow_
      : yEndstopActiveLow_;

  return activeLow ? value == LOW : value == HIGH;
}

bool Motion::home() {
  if (busy_) return false;

  //xMotor.setMaxSpeed(homingSeekRateMmMin_ / 60.0f * xStepsPerMm_);
  setMaxSpeed(xMotor, "X", homingSeekRateMmMin_ / 60.0f * xStepsPerMm_);

  //run the motor in the negative until we hit the switch or run out of space.
  /*
   * X AXIS.
  */
  long traveled = 0;
  while (traveled < HOMING_MAX_SEEK_STEPS_X && !switchPressed(X_MIN_PIN)) {
    long start = xMotor.currentPosition();
    xMotor.move(-HOMING_CHUNK_SIZE);  // relative move toward the switch
    //run the motor a small chunk.
    while (xMotor.distanceToGo() != 0 && !switchPressed(X_MIN_PIN)) {
      xMotor.run();
      yield();
    }

    if (switchPressed(X_MIN_PIN)) break;

    traveled += labs(xMotor.currentPosition() - start);
  }

  //move away from the stop
  xMotor.setCurrentPosition(0);
  xMotor.moveTo(lroundf(HOME_BACKOFF_MM * xStepsPerMm_));

  while (xMotor.distanceToGo() != 0) {
    xMotor.run();
    yield();
  }

  xMotor.setCurrentPosition(0);



  /*
   * Y motor home
  */
  traveled = 0;
  while (traveled < HOMING_MAX_SEEK_STEPS_Y && !switchPressed(Y_MIN_PIN)) {
    long start = yMotor.currentPosition();
    yMotor.move(-HOMING_CHUNK_SIZE);  // relative move toward the switch
    //run the motor a small chunk.
    while (yMotor.distanceToGo() != 0 && !switchPressed(Y_MIN_PIN)) {
      yMotor.run();
      yield();
    }

    if (switchPressed(Y_MIN_PIN)) break;

    traveled += labs(yMotor.currentPosition() - start);
  }

  //move away from the stop
  yMotor.setCurrentPosition(0);
  yMotor.moveTo(lroundf(HOME_BACKOFF_MM * yStepsPerMm_));

  while (yMotor.distanceToGo() != 0) {
    yMotor.run();
    yield();
  }

  yMotor.setCurrentPosition(0);

  xMm_ = yMm_ = 0;
  return true;
}

void Motion::setPosition(float xMm, float yMm) {
  if (busy_) {
    Debugger::log("setPosition rejected: motion is busy");
    return;
  }

  long pos = lroundf(xMm * xStepsPerMm_);
  Debugger::log(String("setCurrentPosition X=") + String(pos));
  xMotor.setCurrentPosition(pos);

  pos = lroundf(yMm * yStepsPerMm_);
  Debugger::log(String("setCurrentPosition Y=") + String(pos));
  yMotor.setCurrentPosition(pos);

  xMm_ = xMm;
  yMm_ = yMm;
}

void Motion::pen(bool up) {
  const int pulse = up ? PEN_UP_US : PEN_DOWN_US;
  penServo.writeMicroseconds(pulse);
  penUp_ = up;
}

void Motion::stop() {
  penUpBeforeStop_ = penUp_;  // capture the state before raising the pen

  xMotor.setCurrentPosition(xMotor.currentPosition());
  yMotor.setCurrentPosition(yMotor.currentPosition());
  xMm_ = xMotor.currentPosition() / xStepsPerMm_;
  yMm_ = yMotor.currentPosition() / yStepsPerMm_;
  busy_ = false;
  movePaused_ = false;
  pen(true);  // stop still safely raises the physical pen
}

void Motion::pause() {
  if (!busy_) return;

  pausedXTarget_ = xMotor.targetPosition();
  pausedYTarget_ = yMotor.targetPosition();

  // Cancel the current targets at the current step positions.
  xMotor.setCurrentPosition(xMotor.currentPosition());
  yMotor.setCurrentPosition(yMotor.currentPosition());

  xMm_ = xMotor.currentPosition() / xStepsPerMm_;
  yMm_ = yMotor.currentPosition() / yStepsPerMm_;
  busy_ = false;
  movePaused_ = true;
}

void Motion::resume() {
  if (!movePaused_) return;
  pausedXTarget_ = xMotor.targetPosition();  // retain your existing logic
  pausedYTarget_ = yMotor.targetPosition();

  moveStartedMs = millis();  // start timing the resumed portion

  xMotor.moveTo(pausedXTarget_);
  yMotor.moveTo(pausedYTarget_);
  busy_ = true;
  movePaused_ = false;
}

bool Motion::setStepsPerMm(float x, float y) {
  if (busy_) return false;
  if (!isfinite(x) || !isfinite(y)) return false;
  if (x < 1.0f || x > 2000.0f || y < 1.0f || y > 2000.0f) return false;

  Preferences preferences;
  if (!preferences.begin("fergoplotter", false)) {
    return false;
  }

  const size_t savedX = preferences.putFloat("x_steps", x);
  const size_t savedY = preferences.putFloat("y_steps", y);
  preferences.end();

  if (savedX != sizeof(float) || savedY != sizeof(float)) {
    return false;
  }

  xStepsPerMm_ = x;
  yStepsPerMm_ = y;

  // Keep the reported coordinates consistent with the current motor steps.
  xMm_ = xMotor.currentPosition() / xStepsPerMm_;
  yMm_ = yMotor.currentPosition() / yStepsPerMm_;
  return true;
}

bool Motion::setMaxRates(float x, float y) {
  if (busy_) return false;
  if (!isfinite(x) || !isfinite(y)) return false;
  if (x < 1.0f || x > 10000.0f || y < 1.0f || y > 10000.0f) return false;

  Preferences preferences;
  if (!preferences.begin("fergoplotter", false)) return false;

  const size_t savedX = preferences.putFloat("x_rate", x);
  const size_t savedY = preferences.putFloat("y_rate", y);
  preferences.end();

  if (savedX != sizeof(float) || savedY != sizeof(float)) return false;

  xMaxRateMmMin_ = x;
  yMaxRateMmMin_ = y;
  return true;
}

bool Motion::setHomingSeekRate(float rate) {
  if (busy_) return false;
  if (!isfinite(rate) || rate < 1.0f || rate > 10000.0f) return false;

  Preferences preferences;
  if (!preferences.begin("fergoplotter", false)) return false;

  const size_t saved = preferences.putFloat("home_rate", rate);
  preferences.end();

  if (saved != sizeof(float)) return false;

  homingSeekRateMmMin_ = rate;
  return true;
}

bool Motion::setEndstopPolarity(bool xActiveLow, bool yActiveLow) {
  xEndstopActiveLow_ = xActiveLow;
  yEndstopActiveLow_ = yActiveLow;
  return true;
}
