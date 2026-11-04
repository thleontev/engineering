#include "autonightlight.h"

#include <math.h>
#include <Wire.h>

bool AutoNightLight::pirDetected() const {
  return pirAllocated && digitalRead(AUTONIGHTLIGHT_PIR_PIN) == HIGH;
}

bool AutoNightLight::dark(uint32_t now) const {
  return enabled && sensor.fresh(now) && sensor.lux() < luxThreshold;
}

void AutoNightLight::fade(uint8_t seconds) {
  uint32_t ms = static_cast<uint32_t>(seconds) * 1000U;
  if (ms > UINT16_MAX) ms = UINT16_MAX;
  transitionDelay = static_cast<uint16_t>(ms);
  jsonTransitionOnce = true;
  strip.setTransition(transitionDelay);
}

bool AutoNightLight::captureState() {
  DynamicJsonDocument doc(JSON_BUFFER_SIZE);
  JsonObject stateJson = doc.to<JsonObject>();
  serializeState(stateJson);
  stateJson["on"] = false;
  stateJson.remove("ps");
  stateJson.remove("pl");
  stateJson.remove("psave");
  stateJson.remove("pdel");
  if (doc.overflowed()) return false;

  previousState = "";
  if (!previousState.reserve(measureJson(doc) + 1)) return false;
  serializeJson(doc, previousState);
  previousPreset = currentPreset;
  previousCycle = presetCycCurr;
  return previousState.length() > 0;
}

void AutoNightLight::restoreStateOff() {
  if (previousState.length() == 0) return;
  DynamicJsonDocument doc(JSON_BUFFER_SIZE);
  if (!deserializeJson(doc, previousState)) {
    JsonObject oldState = doc.as<JsonObject>();
    oldState["on"] = false;
    oldState.remove("ps");
    oldState.remove("pl");
    oldState.remove("psave");
    oldState.remove("pdel");
    ownChange = true;
    deserializeState(oldState, CALL_MODE_NO_NOTIFY);
    bri = 0;
    stateChanged = true;
    stateUpdated(CALL_MODE_NO_NOTIFY);
    if (previousPreset) {
      currentPreset = previousPreset;
      presetCycCurr = previousCycle;
    }
  }
  previousState = "";
}

void AutoNightLight::startManualPause(uint32_t now) {
  state = State::ManualPause;
  pauseStart = now;
}

// Import a preset as a scene, rejecting control commands and missing effects.
// Only the segment configuration is loaded; brightness fades from OFF afterward.
bool AutoNightLight::applyNightScene(uint8_t preset, uint8_t& brightness) {
  if (preset == 0 || bri > 0 || briT > 0) return false;
  DynamicJsonDocument doc(JSON_BUFFER_SIZE);
  if (!readObjectFromFileUsingId(getPresetsFileName(), preset, &doc) || doc.overflowed())
    return false;
  JsonObject src = doc.as<JsonObject>();
  if ((!src["seg"].is<JsonArray>() && !src["seg"].is<JsonObject>()) ||
      !src["win"].isNull() || !src["ps"].isNull() ||
      !src["pl"].isNull() || !src["playlist"].isNull()) return false;

  const int target = src["bri"] | int(briLast ? briLast : briS);
  if (target <= 0 || target > 255) return false;
  bool hasEffect = false;
  auto checkSegment = [&hasEffect](JsonObject seg) -> bool {
    if (seg["stop"].is<int>() && seg["stop"].as<int>() == 0) return true;
    if (!seg["fx"].is<int>()) return false;
    hasEffect = true;
    return true;
  };
  if (src["seg"].is<JsonArray>()) {
    for (JsonVariant item : src["seg"].as<JsonArray>())
      if (!item.is<JsonObject>() || !checkSegment(item.as<JsonObject>())) return false;
  } else if (!checkSegment(src["seg"].as<JsonObject>())) return false;
  if (!hasEffect) return false;

  DynamicJsonDocument scene(JSON_BUFFER_SIZE);
  JsonObject dest = scene.to<JsonObject>();
  dest["seg"] = src["seg"];
  if (!src["mainseg"].isNull()) dest["mainseg"] = src["mainseg"];
  dest["on"] = false;
  dest["bri"] = 0;
  dest["tt"] = 0;
  if (scene.overflowed()) return false;

  // Preparing the segment effect with zero brightness avoids illuminating
  // a partially loaded scene. WLED handles the subsequent fade.
  strip.setTransition(0);
  deserializeState(dest, CALL_MODE_NO_NOTIFY);
  bri = 0;
  brightness = static_cast<uint8_t>(target);
  return true;
}

void AutoNightLight::turnOnByMotion() {
  if (state != State::Idle || bri > 0 || briT > 0) return;
  if (!captureState()) return;

  uint8_t target = briLast ? briLast : briS;
  if (!applyNightScene(onPreset, target)) {
    previousState = "";
    startManualPause(millis()); // avoid rapid retry on a missing preset
    return;
  }
  fade(transitionSec);
  ownChange = true;
  bri = target;
  stateChanged = true;
  stateUpdated(CALL_MODE_BUTTON_PRESET);
  currentPreset = presetCycCurr = onPreset;
  state = State::AutoOn;
  lastLightOn = true;
}

void AutoNightLight::turnOffByMotion() {
  if (state != State::AutoOn && state != State::WaitingOff) return;
  fade(transitionSec);
  ownChange = true;
  briLast = bri;
  bri = 0;
  stateChanged = true;
  stateUpdated(CALL_MODE_BUTTON_PRESET);
  restoreStateOff();
  state = State::Idle;
  lastLightOn = false;
}

void AutoNightLight::setup() {
  if (!pirAllocated && PinManager::allocatePin(AUTONIGHTLIGHT_PIR_PIN, false, PinOwner::UM_PIR)) {
    pinMode(AUTONIGHTLIGHT_PIR_PIN, INPUT);
    pirAllocated = true;
  }
  if (!i2cAllocated) {
    PinManagerPinType pins[2] = {
      {AUTONIGHTLIGHT_SDA_PIN, true}, {AUTONIGHTLIGHT_SCL_PIN, true}
    };
    if (PinManager::allocateMultiplePins(pins, 2, PinOwner::HW_I2C)) {
      i2cAllocated = true;
      Wire.begin(AUTONIGHTLIGHT_SDA_PIN, AUTONIGHTLIGHT_SCL_PIN);
      Wire.setClock(100000);
#ifdef ARDUINO_ARCH_ESP32
      Wire.setTimeOut(50);
#endif
      sensor.begin(Wire, millis());
    }
  }
  lastPir = pirDetected();
  motionArmed = false; // require a measured inactive PIR before motion
  lastLightOn = bri > 0;
  ready = true;
}

void AutoNightLight::loop() {
  if (!ready || !enabled) return;
  const uint32_t now = millis();
  sensor.poll(now);
  if (static_cast<uint32_t>(now - lastPirPoll) < 100U) return;
  lastPirPoll = now;
  const bool pir = pirDetected();
  if (!pir) motionArmed = true;

  if (state == State::ManualPause) {
    if (static_cast<uint32_t>(now - pauseStart) >=
        static_cast<uint32_t>(manualOffPauseSec) * 1000U && !pir) state = State::Idle;
  } else if (state == State::Idle) {
    if (motionArmed && pir && !lastPir && dark(now) && bri == 0 && briT == 0)
      turnOnByMotion();
  } else if (state == State::AutoOn) {
    if (!pir) { state = State::WaitingOff; offStart = now; }
  } else if (state == State::WaitingOff) {
    if (pir) state = State::AutoOn;
    else if (static_cast<uint32_t>(now - offStart) >=
             static_cast<uint32_t>(offDelaySec) * 1000U) turnOffByMotion();
  }
  lastPir = pir;
}

void AutoNightLight::onStateChange(uint8_t mode) {
  (void)mode;
  if (!ready) return;
  const bool isOn = bri > 0;
  const bool own = ownChange;
  ownChange = false;
  if (!own && (state == State::AutoOn || state == State::WaitingOff)) {
    // Any manual state change takes ownership from the automatic scenario.
    if (!isOn) restoreStateOff();
    else previousState = ""; // preserve manual scene, do not auto-off
    startManualPause(millis());
  } else if (!own && lastLightOn && !isOn && enabled && pirDetected()) {
    startManualPause(millis());
  }
  lastLightOn = isOn;
}

void AutoNightLight::addToJsonInfo(JsonObject& root) {
  JsonObject user = root["u"];
  if (user.isNull()) user = root.createNestedObject("u");
  JsonArray values = user.createNestedArray("AutoNightLight");
  values.add(enabled ? "enabled" : "disabled");
  JsonArray lux = user.createNestedArray("Ambient light");
  if (sensor.fresh(millis())) {
    lux.add(sensor.lux());
    lux.add("lx");
  } else lux.add("No data");
  JsonObject status = root.createNestedObject("ambient_light");
  status["sensor"] = "VEML7700";
  status["valid"] = sensor.fresh(millis());
  if (sensor.fresh(millis())) status["lux"] = sensor.lux();
  status["errors"] = sensor.errors();
}

void AutoNightLight::addToConfig(JsonObject& root) {
  JsonObject config = root.createNestedObject("AutoNightLight");
  config["Enabled"] = enabled;
  config["LuxThreshold"] = luxThreshold;
  config["OffDelaySec"] = offDelaySec;
  config["ManualOffPauseSec"] = manualOffPauseSec;
  config["TransitionSec"] = transitionSec;
  config["OnPreset"] = onPreset;
}

bool AutoNightLight::readFromConfig(JsonObject& root) {
  JsonObject config = root["AutoNightLight"];
  if (config.isNull()) return false;
  enabled = config["Enabled"] | enabled;
  const float newThreshold = config["LuxThreshold"] | luxThreshold;
  if (isfinite(newThreshold) && newThreshold >= 0.1f) luxThreshold = newThreshold;
  offDelaySec = constrain(static_cast<int>(config["OffDelaySec"] | int(offDelaySec)), 0, 3600);
  manualOffPauseSec = constrain(static_cast<int>(config["ManualOffPauseSec"] | int(manualOffPauseSec)), 0, 3600);
  transitionSec = constrain(static_cast<int>(config["TransitionSec"] | int(transitionSec)), 0, 60);
  onPreset = constrain(static_cast<int>(config["OnPreset"] | int(onPreset)), 1, 250);
  if (!enabled) {
    previousState = "";
    state = State::Idle;
  }
  return true;
}

static AutoNightLight autoNightLight;
REGISTER_USERMOD(autoNightLight);
