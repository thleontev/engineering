#pragma once

#include <Arduino.h>
#include <Wire.h>

// Minimal non-blocking VEML7700 I2C driver. Nominal conversion coefficients;
// optical geometry and dark offset require calibration in the finished lamp.
class Veml7700 {
  struct Profile {
    uint8_t gainBits;
    uint8_t integrationBits;
    uint16_t integrationMs;
    float luxPerCount;
  };
  static const Profile& profile(uint8_t index) {
    static const Profile profiles[6] = {
      {2, 12, 25, 2.1504f}, {2, 0, 100, 0.5376f},
      {2, 3, 800, 0.0672f}, {3, 3, 800, 0.0336f},
      {0, 3, 800, 0.0084f}, {1, 3, 800, 0.0042f}
    };
    return profiles[index];
  }

  TwoWire* bus = nullptr;
  uint8_t range = 1; // 1/8 gain, 100 ms
  uint32_t configuredAt = 0;
  uint32_t lastPoll = 0;
  uint32_t lastAttempt = 0;
  uint32_t sampledAt = 0;
  uint32_t errorCount = 0;
  float luxValue = 0.0f;
  bool found = false;
  bool valid = false;
  bool saturated = false;
  static constexpr uint8_t address = 0x10;
  static constexpr uint32_t intervalMs = 2000;

  bool readRegister(uint8_t reg, uint16_t& value) {
    bus->beginTransmission(address);
    bus->write(reg);
    if (bus->endTransmission(false) != 0 ||
        bus->requestFrom(address, static_cast<uint8_t>(2)) != 2) {
      ++errorCount;
      return false;
    }
    const uint8_t lo = static_cast<uint8_t>(bus->read());
    const uint8_t hi = static_cast<uint8_t>(bus->read());
    value = (static_cast<uint16_t>(hi) << 8) | lo;
    return true;
  }

  bool configure(uint8_t index, uint32_t now) {
    const Profile& p = profile(index);
    const uint16_t command = (static_cast<uint16_t>(p.gainBits) << 11)
                           | (static_cast<uint16_t>(p.integrationBits) << 6);
    bus->beginTransmission(address);
    bus->write(static_cast<uint8_t>(0x00));
    bus->write(static_cast<uint8_t>(command));
    bus->write(static_cast<uint8_t>(command >> 8));
    if (bus->endTransmission() != 0) { ++errorCount; return false; }
    uint16_t readback = 0;
    if (!readRegister(0x00, readback) || readback != command) {
      ++errorCount;
      return false;
    }
    range = index;
    configuredAt = now;
    lastPoll = now;
    valid = false; // never consume samples from a previous range
    return true;
  }

  bool initialize(uint32_t now) {
    found = valid = false;
    uint16_t id = 0;
    if (!readRegister(0x07, id) || (id & 0xff) != 0x81) {
      ++errorCount;
      return false;
    }
    found = configure(1, now);
    return found;
  }

public:
  bool begin(TwoWire& wire, uint32_t now) {
    bus = &wire;
    lastAttempt = now;
    return initialize(now);
  }

  void poll(uint32_t now) {
    if (!bus) return;
    if (!found) {
      if (static_cast<uint32_t>(now - lastAttempt) < intervalMs) return;
      lastAttempt = now;
      initialize(now);
      return;
    }
    // Allow two integration periods to elapse after changing sensitivity.
    if (static_cast<uint32_t>(now - configuredAt) <
        static_cast<uint32_t>(profile(range).integrationMs) * 2U + 10U) return;
    if (static_cast<uint32_t>(now - lastPoll) < intervalMs) return;
    lastPoll = now;

    uint16_t als = 0, white = 0;
    if (!readRegister(0x04, als) || !readRegister(0x05, white)) {
      found = valid = false;
      lastAttempt = now;
      return;
    }
    (void)white;
    uint8_t next = range;
    if (als >= 12000 && next > 0) --next;
    else if (als < 64 && next < 5) ++next;
    if (next != range) {
      if (!configure(next, now)) {
        found = valid = false;
        lastAttempt = now;
      }
      return;
    }
    saturated = als >= 60000;
    valid = !saturated;
    if (valid) {
      luxValue = als * profile(range).luxPerCount;
      sampledAt = now;
    }
  }

  bool fresh(uint32_t now) const {
    return found && valid && static_cast<uint32_t>(now - sampledAt) <= 4500U;
  }
  bool present() const { return found; }
  float lux() const { return luxValue; }
  uint32_t errors() const { return errorCount; }
  bool isSaturated() const { return saturated; }
};
