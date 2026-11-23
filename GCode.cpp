/******
**.  TINYPenPlotter V2
*/
#include "Gcode.h"
#include "Config.h"
#include <math.h>
#include "Debugger.h"

#include <ctype.h>
#include <stdlib.h>

void startJob() {
  motion_.clearPlanner();

  plannedX_ = motion_.x();
  plannedY_ = motion_.y();
  plannedPositionValid_ = true;
}

static bool wordValue(const String& line, char wantedLetter, float& value) {
  wantedLetter = toupper(wantedLetter);
  bool inParenComment = false;

  for (int i = 0; i < line.length();) {
    const char c = line[i];

    // Semicolon comments continue to the end of the line.
    if (c == ';' && !inParenComment) break;

    // Ignore parenthesized comments.
    if (c == '(') {
      inParenComment = true;
      ++i;
      continue;
    }
    if (c == ')') {
      inParenComment = false;
      ++i;
      continue;
    }
    if (inParenComment) {
      ++i;
      continue;
    }

    // A G-code word begins with a letter, followed immediately by a number.
    if (!isalpha(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }

    const char letter = toupper(c);
    const int numberStart = i + 1;

    char* end = nullptr;
    const float parsed = strtof(line.c_str() + numberStart, &end);

    if (end == line.c_str() + numberStart) {
      // This letter has no numeric value; continue scanning.
      ++i;
      continue;
    }

    if (letter == wantedLetter) {
      value = parsed;
      return true;
    }

    // Move past this word's numeric value, then continue. This also
    // supports compact input such as G1X10Y20.
    i = static_cast<int>(end - line.c_str());
  }

  return false;
}


static bool hasCode(const String& line, const char* code) {
  if (!code || !code[0] || !code[1]) return false;

  const char wantedLetter = toupper(code[0]);
  if (wantedLetter != 'G' && wantedLetter != 'M') return false;

  const float wantedNumber = atof(code + 1);

  // A command letter starts a G/M word. Don't match letters inside comments.
  bool inParenComment = false;

  for (int i = 0; i < line.length(); ++i) {
    const char c = line[i];

    if (c == ';' && !inParenComment) break;
    if (c == '(') {
      inParenComment = true;
      continue;
    }
    if (c == ')') {
      inParenComment = false;
      continue;
    }
    if (inParenComment) continue;

    if (toupper(c) != wantedLetter) continue;

    int j = i + 1;

    // Allow optional whitespace between the command letter and number.
    while (j < line.length() && isspace(line[j])) ++j;

    // Parse the numeric part of the command, including decimals like G38.2.
    const int numberStart = j;
    bool hasDigit = false;
    while (j < line.length() && isdigit(line[j])) {
      hasDigit = true;
      ++j;
    }

    if (j < line.length() && line[j] == '.') {
      ++j;
      while (j < line.length() && isdigit(line[j])) {
        hasDigit = true;
        ++j;
      }
    }

    if (!hasDigit) continue;

    const float foundNumber = line.substring(numberStart, j).toFloat();
    if (fabsf(foundNumber - wantedNumber) < 0.0001f) return true;

    // Skip the rest of this word so letters in its value aren't treated
    // as separate command letters.
    while (j < line.length() && !isspace(line[j]) && line[j] != ';' && line[j] != '(') {
      ++j;
    }
    i = j - 1;
  }

  return false;
}

static bool usbCommandClaimsOwnership(String line) {
  line.trim();
  line.toUpperCase();

  const int comment = line.indexOf(';');
  if (comment >= 0) line = line.substring(0, comment);
  line.trim();

  return hasCode(line, "G0") || hasCode(line, "G1") || hasCode(line, "G28") || line == "M3" || line.startsWith("M3 ") || line == "M4" || line.startsWith("M4 ") || line == "M5" || line.startsWith("M5 ") || line == "M9" || line.startsWith("M9 ") || line.indexOf('Z') >= 0;
}
bool Gcode::enqueue(const String& input, Source source, uint16_t lineNumber) {
  checkpointReady_ = false;
  String line = input;
  line.trim();
  if (!line.length() || line == "?") return true;

  if (ownershipLockEnabled_ && owner_ != NONE && owner_ != source) return false;
  if (count_ >= QUEUE_SIZE) return false;

  if (ownershipLockEnabled_ && owner_ == NONE) {
    // Web explicitly claims control for its job. USB setup/modal commands
    // alone should not lock out web jogging.
    if (source == WEB || usbCommandClaimsOwnership(line)) owner_ = source;
  }

  lastActivityMs_ = millis();

  Entry& e = queue_[tail_];
  e.text = line;
  e.lineNumber = lineNumber;
  e.endsJob = hasCode(line, "M2") || hasCode(line, "M30");
  tail_ = (tail_ + 1) % QUEUE_SIZE;
  ++count_;
  return true;
}


bool Gcode::claimWeb() {
  if (!CONTROL_SOURCE_LOCKING_ENABLED) return true;

  if (owner_ != NONE && owner_ != WEB) return false;
  owner_ = WEB;
  lastActivityMs_ = millis();
  return true;
}
void Gcode::setOwnershipLockEnabled(bool enabled) {
  ownershipLockEnabled_ = enabled;
  if (!enabled) {
    owner_ = NONE;
  } else if (owner_ != NONE) {
    lastActivityMs_ = millis();
  }
  Debugger::log(String("USB/web ownership lock ") + (enabled ? "enabled" : "disabled"));
}


bool Gcode::busy() const {
  return count_ > 0 || motion_.busy() || endPending_;
}

bool Gcode::pause() {
  if (owner_ == NONE) return false;
  paused_ = true;
  motion_.pause();
  return true;
}

void Gcode::resume() {
  motion_.resume();
  paused_ = false;
}

void Gcode::clearQueue() {
  while (count_) {
    queue_[head_].text = "";
    head_ = (head_ + 1) % QUEUE_SIZE;
    --count_;
  }
  tail_ = head_;
}
void Gcode::stop() {
  const bool moveWasBusy = motion_.busy();

  clearQueue();

  if (pendingMotionLineValid_) {
    if (!moveWasBusy) {
      completedLine_ = pendingMotionLine_;
    }

    pendingMotionLine_ = 0;
    pendingMotionLineValid_ = false;
  }

  motion_.stop();
  paused_ = false;
  endPending_ = false;
  owner_ = NONE;
  checkpointReady_ = true;
}

void Gcode::service() {
  if (!plannedPositionValid_) {
    plannedX_ = motion_.x();
    plannedY_ = motion_.y();
    plannedPositionValid_ = true;
  }

  if (ownershipLockEnabled_ && owner_ != NONE && count_ == 0 && !motion_.busy() && !endPending_ && millis() - lastActivityMs_ >= 2000) {
    owner_ = NONE;
  }

  if (motion_.busy()) {
    if (textQueueEmpty()) return;

    const String& nextLine = peekTextQueue();

    if (!isMotionLine(nextLine)) return;
    if (motion_.plannerFull()) return;

    // Now remove this line and parse/enqueue it.
    // Do not remove it before confirming there's planner capacity.
    processNextLine();
    return;
  }

  // Motion::service() has finished the pending move.
  if (pendingMotionLineValid_) {
    completedLine_ = pendingMotionLine_;
    pendingMotionLine_ = 0;
    pendingMotionLineValid_ = false;
  }
  // Do not execute later G-code (such as M5 or M30) until all
  // previously queued planner moves have run.
  if (!motion_.plannerEmpty()) return;
  if (endPending_) {
    endPending_ = false;
    if (count_ == 0) owner_ = NONE;
  }

  if (paused_ || count_ == 0) return;

  Entry e = queue_[head_];
  queue_[head_].text = "";
  head_ = (head_ + 1) % QUEUE_SIZE;
  --count_;

  currentLine_ = e.lineNumber;

  const bool executed = executeLine(e.text);

  if (executed && e.lineNumber != 0) {
    if (motion_.busy()) {
      // The command started a move; commit its line after motion completes.
      pendingMotionLine_ = e.lineNumber;
      pendingMotionLineValid_ = true;
    } else {
      // Non-motion command, home, or no-op completed synchronously.
      completedLine_ = e.lineNumber;
    }
  }

  if (e.endsJob) endPending_ = true;

  if (!motion_.busy() && motion_.plannerEmpty() && endPending_ && count_ == 0) {
    endPending_ = false;
    owner_ = NONE;
  }
}

bool Gcode::executeLine(const String& input) {
  String line = input;
  line.trim();
  line.toUpperCase();

  const int comment = line.indexOf(';');
  if (comment >= 0) {
    line = line.substring(0, comment);
    line.trim();
  }

  if (!line.length()) return true;

  // Runtime steps-per-mm settings.
  if (line.startsWith("$100=")) {
    const float x = line.substring(5).toFloat();
    return motion_.setStepsPerMm(x, motion_.yStepsPerMm());
  }

  if (line.startsWith("$101=")) {
    const float y = line.substring(5).toFloat();
    return motion_.setStepsPerMm(motion_.xStepsPerMm(), y);
  }

  if (line.startsWith("$110=")) {
    const float x = line.substring(5).toFloat();
    return motion_.setMaxRates(x, motion_.yMaxRate());
  }

  if (line.startsWith("$111=")) {
    const float y = line.substring(5).toFloat();
    return motion_.setMaxRates(motion_.xMaxRate(), y);
  }

  if (line.startsWith("$25=")) {
    const float rate = line.substring(4).toFloat();
    return motion_.setHomingSeekRate(rate);
  }

  // Units and distance mode.
  if (hasCode(line, "G20")) inches_ = true;
  if (hasCode(line, "G21")) inches_ = false;
  if (hasCode(line, "G90")) absolute_ = true;
  if (hasCode(line, "G91")) absolute_ = false;

  // Pen commands: M3/M4 down; M5/M9 up.
  if (hasCode(line, "M5") || hasCode(line, "M9")) motion_.pen(true);

  if (hasCode(line, "M3 ") || line == "M3" || hasCode(line, "M4 ") || line == "M4") {
    motion_.pen(false);
  }

  float value;

  // F remains modal for G1 moves.
  if (wordValue(line, 'F', value) && value > 0) {
    feed_ = value;
  }

  if (hasCode(line, "G28")) return motion_.home();

  if (wordValue(line, 'Z', value)) {
    motion_.pen(value > 0);
  }

  // G92 sets the current coordinate without moving.
  if (hasCode(line, "G92")) {
    float x = motion_.x();
    float y = motion_.y();
    const float scale = inches_ ? 25.4f : 1.0f;

    if (wordValue(line, 'X', value)) x = value * scale;
    if (wordValue(line, 'Y', value)) y = value * scale;

    motion_.setPosition(x, y);
    return true;
  }

  const bool rapidMove = hasCode(line, "G0");
  const bool linearMove = hasCode(line, "G1");

  if (!rapidMove && !linearMove) return true;

  float x = motion_.x();
  float y = motion_.y();
  const float scale = inches_ ? 25.4f : 1.0f;

  if (wordValue(line, 'X', value)) {
    x = absolute_ ? value * scale : x + value * scale;
  }

  if (wordValue(line, 'Y', value)) {
    y = absolute_ ? value * scale : y + value * scale;
  }

  // G0 requests the configured rapid maximum.
  // Motion::moveTo() caps that request to the X/Y max rates.
  // G1 uses the explicit or modal F value.
  const float requestedFeed =
    rapidMove ? MAX_FEED_MM_MIN : feed_ * scale;

  if (!motion_.enqueuePlannedMove(x, y, requestedFeed)) {
    return false;
  }

  plannedX_ = x;
  plannedY_ = y;
  return true;
}


String Gcode::status() {
  return String("<") + motion_.state()
         + "|MPos:" + String(motion_.x(), 3)
         + "," + String(motion_.y(), 3)
         + "|FS:0," + String(feed_, 0) + ">";
}

String Gcode::webStatus() {
  String s = "{\"state\":\"";
  s += busy() ? (paused_ ? "Hold" : "Run") : "Idle";
  s += "\",\"busy\":";
  s += busy() ? "true" : "false";
  s += ",\"paused\":";
  s += paused_ ? "true" : "false";
  s += ",\"owner\":\"";
  s += owner_ == USB ? "USB" : owner_ == WEB ? "WEB"
                                             : "NONE";
  s += "\",\"line\":";
  s += String(currentLine_);
  s += ",\"x\":";
  s += String(motion_.x(), 3);
  s += ",\"y\":";
  s += String(motion_.y(), 3);
  s += "}";
  return s;
}
String Gcode::settings() const {
  String result;

  result += "$100=";
  result += String(xStepsPerMm(), 2);
  result += '\n';

  result += "$101=";
  result += String(yStepsPerMm(), 2);
  result += '\n';

  result += "$25=";
  result += String(homingSeekRate(), 2);
  result += '\n';

  result += "$110=";
  result += String(xMaxRate(), 2);
  result += '\n';

  result += "$111=";
  result += String(yMaxRate(), 2);

  return result;
}
static bool isActionableUsbCommand(const String& line) {
  // Commands that can initiate movement or change the pen state.
  return hasCode(line, "G0") || hasCode(line, "G1") || hasCode(line, "G28") || hasCode(line, "M3") || hasCode(line, "M4") || hasCode(line, "M5") || hasCode(line, "M9") || line.indexOf('Z') >= 0;
}

void Gcode::resetCheckpoint() {
  completedLine_ = 0;
  pendingMotionLine_ = 0;
  pendingMotionLineValid_ = false;
  checkpointReady_ = false;
}

String Gcode::checkpointJson() const {
  String result = "{\"valid\":";
  result += checkpointReady_ ? "true" : "false";
  result += ",\"completedLine\":";
  result += String(completedLine_);
  result += ",\"x\":";
  result += String(motion_.x(), 3);
  result += ",\"y\":";
  result += String(motion_.y(), 3);
  result += ",\"penUp\":";
  result += motion_.penUpBeforeStop() ? "true" : "false";
  result += "}";

  return result;
}
