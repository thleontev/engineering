#pragma once

#include "wled.h"

#ifndef USERMOD_ID_LUMACORE_PIR_BH1750
#define USERMOD_ID_LUMACORE_PIR_BH1750 1001
#endif

#ifndef LUMACORE_PIR_PIN
#define LUMACORE_PIR_PIN 4
#endif

#ifndef LUMACORE_BH1750_SDA_PIN
#define LUMACORE_BH1750_SDA_PIN 16
#endif

#ifndef LUMACORE_BH1750_SCL_PIN
#define LUMACORE_BH1750_SCL_PIN 17
#endif

#ifndef LUMACORE_BH1750_ADDRESS
#define LUMACORE_BH1750_ADDRESS 0x23
#endif

#ifndef LUMACORE_LUX_READ_INTERVAL_MS
#define LUMACORE_LUX_READ_INTERVAL_MS 2000
#endif

#ifndef LUMACORE_PIR_ACTIVE_HIGH
#define LUMACORE_PIR_ACTIVE_HIGH 1
#endif

class LumaCorePirBh1750Usermod : public Usermod {
  private:
    enum class NightLightState : uint8_t {
      Disabled,
      Idle,
      AutoOn,
      WaitingOff,
      ManualOverride
    };

    static const char _name[];
    static const char _legacyName[];
    static const char _enabled[];
    static const char _luxThreshold[];
    static const char _offDelaySec[];
    static const char _transitionSec[];
    static const char _onPreset[];

    bool enabled = true;
    int8_t pirPin = LUMACORE_PIR_PIN;
    bool pirActiveHigh = LUMACORE_PIR_ACTIVE_HIGH != 0;
    float luxThreshold = 5.0f;
    uint16_t offDelaySec = 120;
    uint8_t transitionSec = 2;
    uint8_t onPreset = 2;
    int8_t sdaPin = LUMACORE_BH1750_SDA_PIN;
    int8_t sclPin = LUMACORE_BH1750_SCL_PIN;
    uint8_t bh1750Address = LUMACORE_BH1750_ADDRESS;
    uint16_t luxReadIntervalMs = LUMACORE_LUX_READ_INTERVAL_MS;

    bool initDone = false;
    bool pirPinAllocated = false;
    bool pirPinAllocationFailed = false;
    bool i2cPinsAllocated = false;
    bool sensorFound = false;
    bool luxValid = false;
    bool rawPirHigh = false;
    bool pirActive = false;
    bool lastPirActive = false;
    NightLightState nightLightState = NightLightState::Idle;
    bool ownStateChange = false;
    bool lastWledOn = false;
    bool previousStateSaved = false;
    bool restoreStateAfterManualOff = false;

    float lastLux = 0.0f;
    unsigned long lastPirCheck = 0;
    unsigned long lastLuxRead = 0;
    unsigned long offTimerStart = 0;
    char* previousStateJson = nullptr;
    size_t previousStateJsonLen = 0;
    byte previousCurrentPreset = 0;
    byte previousPresetCycCurr = 0;
    int16_t previousCurrentPlaylist = -1;

    bool initI2c();
    bool initBh1750();
    bool writeBh1750Command(uint8_t command);
    bool readBh1750Lux();
    bool capturePreviousState();
    bool restorePreviousStateKeepingOff();
    void clearPreviousState();
    void readPir();
    void handlePir();
    void switchOnByPir();
    void switchOffByPir();
    void setOneShotTransition(uint8_t seconds);
    bool isDark() const;
    bool isPirOwnedState() const;
    void setNightLightState(NightLightState newState);
    void releasePins(int8_t oldPirPin, int8_t oldSdaPin, int8_t oldSclPin, bool releasePirPin, bool releaseI2cPins);

  public:
    void setup() override;
    void loop() override;
    void onStateChange(uint8_t mode) override;
    void addToJsonInfo(JsonObject& root) override;
    void addToConfig(JsonObject& root) override;
    bool readFromConfig(JsonObject& root) override;
    uint16_t getId() override { return USERMOD_ID_LUMACORE_PIR_BH1750; }
};
