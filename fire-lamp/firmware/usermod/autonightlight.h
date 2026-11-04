#pragma once

#include "wled.h"
#include "veml7700.h"

#ifndef AUTONIGHTLIGHT_PIR_PIN
#define AUTONIGHTLIGHT_PIR_PIN 4
#endif
#ifndef AUTONIGHTLIGHT_SDA_PIN
#define AUTONIGHTLIGHT_SDA_PIN 47
#endif
#ifndef AUTONIGHTLIGHT_SCL_PIN
#define AUTONIGHTLIGHT_SCL_PIN 48
#endif

// WLED v0.16.x Usermod API; independent of private firmware services.
class AutoNightLight : public Usermod {
  enum class State : uint8_t { Idle, AutoOn, WaitingOff, ManualPause };

  bool enabled = true;
  float luxThreshold = 5.0f;
  uint16_t offDelaySec = 120;
  uint16_t manualOffPauseSec = 600;
  uint8_t transitionSec = 2;
  uint8_t onPreset = 2;

  Veml7700 sensor;
  State state = State::Idle;
  bool ready = false;
  bool pirAllocated = false;
  bool i2cAllocated = false;
  bool lastPir = false;
  bool lastLightOn = false;
  bool motionArmed = false;
  bool ownChange = false;
  uint32_t lastPirPoll = 0;
  uint32_t offStart = 0;
  uint32_t pauseStart = 0;

  String previousState;
  uint8_t previousPreset = 0;
  uint8_t previousCycle = 0;

  bool dark(uint32_t now) const;
  bool captureState();
  void restoreStateOff();
  void startManualPause(uint32_t now);
  void turnOnByMotion();
  void turnOffByMotion();
  void fade(uint8_t seconds);
  bool applyNightScene(uint8_t preset, uint8_t& brightness);
  bool pirDetected() const;

public:
  void setup() override;
  void loop() override;
  void onStateChange(uint8_t mode) override;
  void addToJsonInfo(JsonObject& root) override;
  void addToConfig(JsonObject& root) override;
  bool readFromConfig(JsonObject& root) override;
  uint16_t getId() override { return 1001; }
};
