#include "lumacore_light_scale.h"
#include "usermod_lumacore_pir_bh1750.h"

#include <Wire.h>
#include "lumacore_night_scene.h"

static constexpr uint16_t LUMACORE_PIR_CHECK_INTERVAL_MS = 100;
bool LumaCorePirBh1750Usermod::initI2c()
{
  sensorFound = false;
  luxValid = false;

  if (sdaPin < 0 || sclPin < 0) return false;

  PinManagerPinType i2cPins[2] = { { sdaPin, true }, { sclPin, true } };
  if (!PinManager::allocateMultiplePins(i2cPins, 2, PinOwner::HW_I2C)) {
    DEBUG_PRINTLN(F("LumaCore: could not allocate I2C pins."));
    return false;
  }

  i2cPinsAllocated = true;
  Wire.begin(sdaPin, sclPin);
  Wire.setClock(100000);
#ifdef ARDUINO_ARCH_ESP32
  Wire.setTimeOut(50);
#endif
  return true;
}

// Synchronize one selected sensor snapshot with PIR and OTA logic.
// Do not reuse an old lux value after a disconnect, range change or timeout.
void LumaCorePirBh1750Usermod::syncLightReading(uint32_t now)
{
  const bool previouslyPresent = sensorFound;
  sensorFound = ambientLight.present();
  luxValid = enabled && ambientLight.fresh(now, static_cast<uint32_t>(luxReadIntervalMs) * 2U + 500U);
  if (luxValid) {
    lastLux = ambientLight.reading().lux;
    lastLuxRead = ambientLight.reading().sampledAt;
  }
}

void LumaCorePirBh1750Usermod::readPir()
{
  if (!pirPinAllocated) {
    rawPirHigh = false;
    pirActive = false;
    return;
  }

  rawPirHigh = digitalRead(pirPin) == HIGH;
  pirActive = pirActiveHigh ? rawPirHigh : !rawPirHigh;
}

bool LumaCorePirBh1750Usermod::isDark() const
{
  return enabled && luxValid && ambientLight.fresh(millis(), static_cast<uint32_t>(luxReadIntervalMs) * 2U + 500U) && lastLux < luxThreshold;
}

bool LumaCorePirBh1750Usermod::isPirOwnedState() const
{
  return nightLightState == NightLightState::AutoOn || nightLightState == NightLightState::WaitingOff;
}

void LumaCorePirBh1750Usermod::setNightLightState(NightLightState newState)
{
  const NightLightState previousState = nightLightState;
  nightLightState = newState;

  if (newState == NightLightState::WaitingOff) {
    if (!offTimerStart) offTimerStart = millis();
  } else {
    offTimerStart = 0;
  }

  if (newState == NightLightState::ManualOverride) {
    if (previousState != NightLightState::ManualOverride) manualOverrideStart = millis();
  } else {
    manualOverrideStart = 0;
  }
}

void LumaCorePirBh1750Usermod::clearPreviousState()
{
  p_free(previousStateJson);
  previousStateJson = nullptr;
  previousStateJsonLen = 0;
  previousStateSaved = false;
  previousCurrentPreset = 0;
  previousPresetCycCurr = 0;
  previousCurrentPlaylist = -1;
}

bool LumaCorePirBh1750Usermod::capturePreviousState()
{
  clearPreviousState();

  DynamicJsonDocument stateDoc(JSON_BUFFER_SIZE);
  JsonObject state = stateDoc.to<JsonObject>();
  serializeState(state);
  state["on"] = false;
  state.remove(F("ps"));
  state.remove(F("pl"));
  state.remove(F("psave"));
  state.remove(F("pdel"));

  if (stateDoc.overflowed()) {
    DEBUG_PRINTLN(F("LumaCore: previous state snapshot overflowed."));
    clearPreviousState();
    return false;
  }

  previousCurrentPreset = currentPreset;
  previousPresetCycCurr = presetCycCurr;
  previousCurrentPlaylist = currentPlaylist;
  previousStateJsonLen = measureJson(stateDoc) + 1;
  previousStateJson = static_cast<char*>(p_malloc(previousStateJsonLen));
  if (!previousStateJson) {
    DEBUG_PRINTLN(F("LumaCore: could not allocate previous state snapshot."));
    clearPreviousState();
    return false;
  }

  serializeJson(stateDoc, previousStateJson, previousStateJsonLen);
  previousStateSaved = true;
  return true;
}

bool LumaCorePirBh1750Usermod::restorePreviousStateKeepingOff()
{
  if (!previousStateSaved || !previousStateJson) return false;

  byte restoredPreset = previousCurrentPreset;
  byte restoredPresetCyc = previousPresetCycCurr;
  int16_t restoredPlaylist = previousCurrentPlaylist;

  PSRAMDynamicJsonDocument stateDoc(JSON_BUFFER_SIZE);
  DeserializationError error = deserializeJson(stateDoc, previousStateJson);
  if (error) {
    DEBUG_PRINTLN(F("LumaCore: could not deserialize previous state snapshot."));
    clearPreviousState();
    return false;
  }

  JsonObject state = stateDoc.as<JsonObject>();
  state["on"] = false;
  state.remove(F("ps"));
  state.remove(F("pl"));
  state.remove(F("psave"));
  state.remove(F("pdel"));

  ownStateChange = true;
  deserializeState(state, CALL_MODE_NO_NOTIFY);

  if (bri > 0) {
    briLast = bri;
    bri = 0;
    stateChanged = true;
    ownStateChange = true;
    stateUpdated(CALL_MODE_NO_NOTIFY);
  }

  if (restoredPlaylist < 0 && restoredPreset > 0) {
    currentPreset = restoredPreset;
    presetCycCurr = restoredPresetCyc;
  }

  clearPreviousState();
  return true;
}

void LumaCorePirBh1750Usermod::setOneShotTransition(uint8_t seconds)
{
  uint32_t duration = static_cast<uint32_t>(seconds) * 1000U;
  if (duration > UINT16_MAX) duration = UINT16_MAX;
  transitionDelay = static_cast<uint16_t>(duration);
  jsonTransitionOnce = true;
  strip.setTransition(transitionDelay);
}

void LumaCorePirBh1750Usermod::switchOnByPir()
{
  if (bri > 0 || briT > 0 || nightLightState != NightLightState::Idle) return;
  if (!capturePreviousState()) return;

    byte nightBrightness = briLast ? briLast : briS;
  if (onPreset > 0) {
    ownStateChange = true;
    if (!lumacore::prepareNightPresetWhileDark(onPreset, nightBrightness)) {
      ownStateChange = false;
      clearPreviousState();
      // Avoid repeated FS/NVS attempts while the same PIR level remains active.
      setNightLightState(NightLightState::ManualOverride);
      return;
    }
  }
  // All segment changes are complete at zero output. Fade only this scene in.
  setOneShotTransition(transitionSec);
  ownStateChange = true;
  bri = nightBrightness;
  strip.restartRuntime();
  stateChanged = true;
  stateUpdated(CALL_MODE_BUTTON_PRESET);
  if (onPreset > 0) currentPreset = presetCycCurr = onPreset;
  
  setNightLightState(NightLightState::AutoOn);
}

void LumaCorePirBh1750Usermod::switchOffByPir()
{
  if (!isPirOwnedState()) return;

  setOneShotTransition(transitionSec);
  ownStateChange = true;

  if (bri > 0) {
    briLast = bri;
    bri = 0;
    stateChanged = true;
    stateUpdated(CALL_MODE_BUTTON_PRESET);
  }

  restorePreviousStateKeepingOff();
  setNightLightState(NightLightState::Idle);
}

void LumaCorePirBh1750Usermod::handlePir()
{
  if (!enabled) {
    setNightLightState(NightLightState::Disabled);
    return;
  }

  if (nightLightState == NightLightState::Disabled) setNightLightState(NightLightState::Idle);
  if (!pirPinAllocated) return;

  if (pirActive != lastPirActive) {
    DEBUG_PRINTF_P(PSTR("LumaCore: PIR GPIO%d %s\n"), pirPin, pirActive ? PSTR("detected") : PSTR("none"));
    lastPirActive = pirActive;
  }

  // Do not light up immediately after a power restoration with PIR held high.
  // Motion must first become inactive and then active again in this boot.
  if (!pirActive) motionArmedAfterBoot = true;

  switch (nightLightState) {
    case NightLightState::Disabled:
      return;

    case NightLightState::Idle:
      if (!motionArmedAfterBoot || !pirActive || !isDark() || bri > 0) return;
      switchOnByPir();
      return;

    case NightLightState::AutoOn:
      if (pirActive) return;
      setNightLightState(NightLightState::WaitingOff);
      break;

    case NightLightState::WaitingOff:
      if (pirActive) {
        setNightLightState(NightLightState::AutoOn);
        return;
      }
      break;

    case NightLightState::ManualOverride: {
      const unsigned long pauseMs = static_cast<unsigned long>(manualOffPauseSec) * 1000UL;
      const bool pauseExpired = manualOffPauseSec == 0 || millis() - manualOverrideStart >= pauseMs;
      if (pauseExpired && !pirActive) setNightLightState(NightLightState::Idle);
      return;
    }
  }

  if (nightLightState == NightLightState::WaitingOff && millis() - offTimerStart >= static_cast<unsigned long>(offDelaySec) * 1000UL) {
    switchOffByPir();
  }
}

void LumaCorePirBh1750Usermod::releasePins(int8_t oldPirPin, int8_t oldSdaPin, int8_t oldSclPin, bool releasePirPin, bool releaseI2cPins)
{
  if (releasePirPin && oldPirPin >= 0) {
    PinManager::deallocatePin(oldPirPin, PinOwner::UM_PIR);
  }

  if (releaseI2cPins) {
    uint8_t pins[2] = { static_cast<uint8_t>(oldSdaPin), static_cast<uint8_t>(oldSclPin) };
    PinManager::deallocateMultiplePins(pins, 2, PinOwner::HW_I2C);
  }
}


void LumaCorePirBh1750Usermod::setup()
{
  pirPinAllocated = false;
  pirPinAllocationFailed = false;
  i2cPinsAllocated = false;
  sensorFound = false;
  luxValid = false;
  rawPirHigh = false;
  pirActive = false;
  lastPirActive = false;
  motionArmedAfterBoot = false;

  if (!enabled) setNightLightState(NightLightState::Disabled);
  else if (nightLightState == NightLightState::Disabled) setNightLightState(NightLightState::Idle);
  else if (nightLightState == NightLightState::WaitingOff && !offTimerStart) offTimerStart = millis();
  else if (nightLightState != NightLightState::WaitingOff) offTimerStart = 0;

  if (enabled && pirPin >= 0) {
    if (PinManager::allocatePin(pirPin, false, PinOwner::UM_PIR)) {
      pinMode(pirPin, INPUT);
      pirPinAllocated = true;
      readPir();
      lastPirActive = pirActive;
    } else {
      DEBUG_PRINTLN(F("LumaCore: could not allocate PIR pin."));
      pirPinAllocationFailed = true;
    }
  }

  // Selected driver begins only when the controlled night-light is enabled.
  if (enabled && initI2c()) {
    ambientLight.begin(Wire, millis());
    syncLightReading(millis());
  }

  lastWledOn = bri > 0;
  lastLuxRead = millis();
  initDone = true;
}

void LumaCorePirBh1750Usermod::loop()
{
  if (restoreStateAfterManualOff) {
    restoreStateAfterManualOff = false;
    restorePreviousStateKeepingOff();
    lastWledOn = bri > 0;
  }

  if (!enabled) {
    setNightLightState(NightLightState::Disabled);
    return;
  }
  if (nightLightState == NightLightState::Disabled) setNightLightState(NightLightState::Idle);

  unsigned long now = millis();
  // Polling is nonblocking; VEML7700 integrations and range settle on millis().
  ambientLight.poll(now);
  syncLightReading(now);

  if (now - lastPirCheck < LUMACORE_PIR_CHECK_INTERVAL_MS) return;
  lastPirCheck = now;

  readPir();
  handlePir();
}

void LumaCorePirBh1750Usermod::onStateChange(uint8_t mode)
{
  if (!initDone) return;

  bool currentWledOn = bri > 0;
  bool ignoreOwnChange = ownStateChange;
  ownStateChange = false;

  if (!ignoreOwnChange && lastWledOn && !currentWledOn) {
    if (enabled && pirPinAllocated) readPir();
    if (isPirOwnedState()) {
      setNightLightState(NightLightState::ManualOverride);
      restoreStateAfterManualOff = true;
    } else if (enabled && nightLightState == NightLightState::Idle && pirActive) {
      setNightLightState(NightLightState::ManualOverride);
    }
  }

  lastWledOn = currentWledOn;
}

void LumaCorePirBh1750Usermod::addToJsonInfo(JsonObject& root)
{
  JsonObject user = root[F("u")];
  if (user.isNull()) user = root.createNestedObject(F("u"));

  JsonArray status = user.createNestedArray(F("Night Light"));
  status.add(enabled ? F("enabled") : F("disabled"));

  JsonArray lux = user.createNestedArray(F("  light"));
  syncLightReading(millis());
  char luxText[32];
  lumacore::formatLightLux(luxText, sizeof(luxText), luxValid, lastLux);
  lux.add(luxText);

  JsonArray dark = user.createNestedArray(F("  dark"));
  dark.add(isDark() ? F("yes") : F("no"));

  JsonArray motion = user.createNestedArray(F("  motion"));
  motion.add(pirActive ? F("detected") : F("none"));

  JsonArray state = user.createNestedArray(F("  state"));
  switch (enabled ? nightLightState : NightLightState::Disabled) {
    case NightLightState::Disabled:
      state.add(F("disabled"));
      break;
    case NightLightState::Idle:
      state.add(F("idle"));
      break;
    case NightLightState::AutoOn:
      state.add(F("auto-on"));
      break;
    case NightLightState::WaitingOff:
      state.add(F("waiting off"));
      break;
    case NightLightState::ManualOverride:
      state.add(F("manual override"));
      break;
  }

  JsonObject sensorInfo = root.createNestedObject(F("ambient_light"));
  sensorInfo[F("sensor")] = ambientLight.name();
  sensorInfo[F("status")] = sensorFound && !luxValid && ambientLight.reading().valid ? "stale" : ambientLight.status();
  sensorInfo[F("valid")] = luxValid;
  if (luxValid) sensorInfo[F("lux")] = lastLux;

#ifdef LUMACORE_DEBUG_INFO
  JsonArray pirPinInfo = user.createNestedArray(F("Night Light debug pirPin"));
  pirPinInfo.add(pirPin);

  JsonArray rawPirLevel = user.createNestedArray(F("Night Light debug rawPirLevel"));
  rawPirLevel.add(rawPirHigh ? F("HIGH") : F("LOW"));

  JsonArray pirActiveHighInfo = user.createNestedArray(F("Night Light debug pirActiveHigh"));
  pirActiveHighInfo.add(pirActiveHigh);

  JsonArray pirDetected = user.createNestedArray(F("Night Light debug pirDetected"));
  pirDetected.add(pirActive);

  JsonArray owned = user.createNestedArray(F("Night Light debug switchedOnByPir"));
  owned.add(isPirOwnedState());

  JsonArray manual = user.createNestedArray(F("Night Light debug manualOverride"));
  manual.add(nightLightState == NightLightState::ManualOverride);
#endif
}

void LumaCorePirBh1750Usermod::appendConfigData() {
  // Form values, persisted LuxThreshold and sensor calculations all use physical lux.
  oappend(R"LCSCALE((function(){
const fields=Array.from(d.getElementsByName('AutoNightLight:LuxThreshold'));
const input=fields.find(e=>e.type==='number');
if(!input)return;
input.min='0.1';input.step='0.1';
addInfo('AutoNightLight:LuxThreshold',1,'lux');
})();)LCSCALE");
}

void LumaCorePirBh1750Usermod::addToConfig(JsonObject& root)
{
  JsonObject top = root.createNestedObject(FPSTR(_name));
  // Service-only takeover suspends the usermod for this session, not in shared cfg.json.
  top[FPSTR(_luxThreshold)] = luxThreshold;
  top[FPSTR(_offDelaySec)] = offDelaySec;
  top[FPSTR(_manualOffPauseSec)] = manualOffPauseSec;
  top[FPSTR(_transitionSec)] = transitionSec;
  top[FPSTR(_onPreset)] = onPreset;
}

bool LumaCorePirBh1750Usermod::readFromConfig(JsonObject& root)
{
  const float previousLuxThreshold = luxThreshold;
  int8_t oldPirPin = pirPin;
  int8_t oldSdaPin = sdaPin;
  int8_t oldSclPin = sclPin;
  bool oldPirPinAllocated = pirPinAllocated;
  bool oldI2cPinsAllocated = i2cPinsAllocated;

  JsonObject top = root[FPSTR(_name)];
  bool usingLegacyConfig = top.isNull();
  if (usingLegacyConfig) top = root[FPSTR(_legacyName)];
  bool configComplete = !top.isNull() && !usingLegacyConfig;

  if (!top[FPSTR(_enabled)].isNull()) configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled, true);
  else {
    configComplete &= getJsonValue(top[F("enabled")], enabled, true);
    configComplete = false;
  }

  if (!top[F("LuxThresholdTenths")].isNull()) {
    luxThreshold = lumacore::thresholdLuxFromUnits(top[F("LuxThresholdTenths")].as<float>());
  } else if (!top[FPSTR(_luxThreshold)].isNull()) configComplete &= getJsonValue(top[FPSTR(_luxThreshold)], luxThreshold, 5.0f);
  else {
    configComplete &= getJsonValue(top[F("luxThreshold")], luxThreshold, 5.0f);
    configComplete = false;
  }

  if (!top[FPSTR(_offDelaySec)].isNull()) configComplete &= getJsonValue(top[FPSTR(_offDelaySec)], offDelaySec, 120);
  else {
    configComplete &= getJsonValue(top[F("offDelaySec")], offDelaySec, 120);
    configComplete = false;
  }

  if (!top[FPSTR(_manualOffPauseSec)].isNull()) {
    configComplete &= getJsonValue(top[FPSTR(_manualOffPauseSec)], manualOffPauseSec, 600);
  } else {
    configComplete &= getJsonValue(top[F("manualOffPauseSec")], manualOffPauseSec, 600);
    configComplete = false;
  }

  if (!top[FPSTR(_transitionSec)].isNull()) {
    configComplete &= getJsonValue(top[FPSTR(_transitionSec)], transitionSec, 2);
  } else {
    uint8_t migratedTransitionSec = 2;
    if (!top[F("fadeInSec")].isNull()) getJsonValue(top[F("fadeInSec")], migratedTransitionSec, 2);
    else if (!top[F("fadeOutSec")].isNull()) getJsonValue(top[F("fadeOutSec")], migratedTransitionSec, 2);
    transitionSec = migratedTransitionSec;
    configComplete = false;
  }

  if (!top[FPSTR(_onPreset)].isNull()) configComplete &= getJsonValue(top[FPSTR(_onPreset)], onPreset, 2);
  else {
    configComplete &= getJsonValue(top[F("onPreset")], onPreset, 2);
    configComplete = false;
  }

  pirPin = LUMACORE_PIR_PIN;
  pirActiveHigh = LUMACORE_PIR_ACTIVE_HIGH != 0;
  sdaPin = LUMACORE_LIGHT_SDA_PIN;
  sclPin = LUMACORE_LIGHT_SCL_PIN;
  luxReadIntervalMs = LUMACORE_LUX_READ_INTERVAL_MS;

  luxThreshold = lumacore::thresholdLuxOrPrevious(luxThreshold, previousLuxThreshold);
  if (offDelaySec > 3600) offDelaySec = 3600;
  if (manualOffPauseSec > 3600) manualOffPauseSec = 3600;
  if (transitionSec > 60) transitionSec = 60;
  if (onPreset > 250) onPreset = 250;

  if (initDone) {
    releasePins(oldPirPin, oldSdaPin, oldSclPin, oldPirPinAllocated, oldI2cPinsAllocated);
    setup();
  }

  return configComplete;
}

const char LumaCorePirBh1750Usermod::_name[] PROGMEM = "AutoNightLight";
const char LumaCorePirBh1750Usermod::_legacyName[] PROGMEM = "LumaCorePIRBH1750";
const char LumaCorePirBh1750Usermod::_enabled[] PROGMEM = "Enabled";
const char LumaCorePirBh1750Usermod::_luxThreshold[] PROGMEM = "LuxThreshold";
const char LumaCorePirBh1750Usermod::_offDelaySec[] PROGMEM = "OffDelaySec";
const char LumaCorePirBh1750Usermod::_manualOffPauseSec[] PROGMEM = "ManualOffPauseSec";
const char LumaCorePirBh1750Usermod::_transitionSec[] PROGMEM = "TransitionSec";
const char LumaCorePirBh1750Usermod::_onPreset[] PROGMEM = "OnPreset";

static LumaCorePirBh1750Usermod lumacore_pir_bh1750;
REGISTER_USERMOD(lumacore_pir_bh1750);
