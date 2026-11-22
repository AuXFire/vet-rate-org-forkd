// World clock and weather. Game time runs faster than real time (default: one in-game day = 48 real minutes).
#pragma once

#include <cstdint>

#include "Core/UtEvents.h"
#include "Core/UtJson.h"
#include "Core/UtRng.h"

namespace ut {

struct Clock {
    float secondsOfDay = 8.0f * 3600.0f;
    int day = 1;
    float timeScale = 30.0f;   // game seconds per real second

    // Advances by real seconds. Returns the game seconds elapsed; emits DayStarted at midnight.
    float advance(float realDt, EventQueue& events);
    float hour() const { return secondsOfDay / 3600.0f; }
    // 0 at night, 1 in full daylight, with one-hour ramps at dawn (06-07) and dusk (19-20).
    float daylight() const;
    bool isNight() const { return daylight() < 0.5f; }

    Json save() const;
    void load(const Json& j);
};

enum class Weather : uint8_t { Clear, Overcast, Rain, Storm, Fog, Count };

const char* weatherName(Weather w);

class WeatherSystem {
public:
    // `gameDt` is game seconds (Clock::advance's return value).
    void update(float gameDt, Rng& rng, EventQueue& events);
    void force(Weather w, EventQueue& events);

    Weather current() const { return current_; }
    float wetness() const { return wetness_; }                 // 0 dry .. 1 soaked; lags behind the weather
    float grip() const { return 1.0f - 0.35f * wetness_; }      // tyre grip multiplier for the vehicle layer
    float visibility() const;                                   // 0..1, drives fog density and AI sight range
    float pedestrianFactor() const;                             // fewer people outside in bad weather
    float trafficFactor() const;

    Json save() const;
    void load(const Json& j);

private:
    Weather current_ = Weather::Overcast;
    float wetness_ = 0.0f;
    float untilNextRoll_ = 3600.0f;   // game seconds
};

}  // namespace ut
