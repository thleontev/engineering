#include "wled.h"
#include "lumacore_night_scene.h"

namespace lumacore {
bool prepareNightPresetWhileDark(uint8_t preset, uint8_t& brightness) {
  // A manual preset request or an unfinished OFF fade has priority.
  if (!preset || bri || briT) return false;
  DynamicJsonDocument document(JSON_BUFFER_SIZE);
  if (!readObjectFromFileUsingId(getPresetsFileName(), preset, &document)
      || document.overflowed()) return false;
  JsonObject state = document.as<JsonObject>();
  if ((!state["seg"].is<JsonArray>() && !state["seg"].is<JsonObject>())
      || !state["win"].isNull() || !state["ps"].isNull()
      || !state["pl"].isNull() || !state["playlist"].isNull()) return false;
  const int target = state["bri"] | int(briLast ? briLast : briS);
  if (target <= 0 || target > 255) return false;
  bool hasEffect = false;
  auto prepareSegment = [&hasEffect](JsonObject segment) -> bool {
    if (segment["stop"].is<int>() && segment["stop"].as<int>() == 0) return true;
    // A brightness-only preset could otherwise turn the old effect back on.
    if (!segment["fx"].is<uint8_t>()) return false;
    hasEffect = true;
    if (segment["on"].isNull()) segment["on"] = true;
    return true;
  };
  if (state["seg"].is<JsonArray>()) {
    for (JsonVariant item : state["seg"].as<JsonArray>()) {
      if (!item.is<JsonObject>() || !prepareSegment(item.as<JsonObject>())) return false;
    }
  } else if (!prepareSegment(state["seg"].as<JsonObject>())) return false;
  if (!hasEffect) return false;

  // Apply only a scene, without commands, timers or preset transition overrides.
  for (JsonObject::iterator it = state.begin(); it != state.end();) {
    const char* key = it->key().c_str();
    ++it;
    if (strcmp(key, "seg") && strcmp(key, "mainseg") && strcmp(key, "bs")) state.remove(key);
  }
  state["on"] = false;
  state["bri"] = 0;
  state["tt"] = 0;
  unloadPlaylist();
  strip.suspend();
  strip.waitForIt();
  strip.setTransition(0);
  // Discard old-effect copies, including an interrupted previous transition.
  for (size_t i = 0; i < strip.getSegmentsNum(); ++i) {
    Segment& segment = strip.getSegment(i);
    segment.finishTransition();
    // Unmentioned segments must not illuminate with their old user effect.
    segment.setOption(SEG_OPTION_ON, false);
  }
  deserializeState(state, CALL_MODE_NO_NOTIFY);
  bri = briT = briOld = 0;
  strip.setBrightness(0);
  strip.restartRuntime();
  currentPreset = presetCycCurr = preset;
  strip.resume();
  brightness = static_cast<uint8_t>(target);
  return true;
}
}
