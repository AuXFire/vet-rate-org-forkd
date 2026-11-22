#include "Core/UtClock.h"

namespace ut {

float Clock::advance(float realDt, EventQueue& events) {
    const float gameDt = realDt * timeScale;
    secondsOfDay += gameDt;
    while (secondsOfDay >= 86400.0f) {
        secondsOfDay -= 86400.0f;
        ++day;
        events.push(EventType::DayStarted, day);
    }
    return gameDt;
}

float Clock::daylight() const {
    const float h = hour();
    if (h < 6.0f || h >= 20.0f) return 0.0f;
    if (h < 7.0f) return h - 6.0f;
    if (h < 19.0f) return 1.0f;
    return 20.0f - h;
}

Json Clock::save() const {
    Json j = Json::object();
    j.set("seconds", secondsOfDay);
    j.set("day", day);
    return j;
}

void Clock::load(const Json& j) {
    secondsOfDay = clampf(j["seconds"].asFloat(secondsOfDay), 0.0f, 86399.0f);
    day = j["day"].asInt(day);
}

const char* weatherName(Weather w) {
    switch (w) {
        case Weather::Clear: return "clear";
        case Weather::Overcast: return "overcast";
        case Weather::Rain: return "rain";
        case Weather::Storm: return "storm";
        case Weather::Fog: return "fog";
        case Weather::Count: break;
    }
    return "clear";
}

namespace clock_detail {

// Hourly transition weights. Port Meridian is a northern port: overcast and rain dominate, clear spells are short.
constexpr int kWeatherCount = static_cast<int>(Weather::Count);
constexpr float kTransitions[kWeatherCount][kWeatherCount] = {
    // to:  Clear  Overcast Rain   Storm  Fog
    {0.70f, 0.24f, 0.02f, 0.00f, 0.04f},   // from Clear
    {0.14f, 0.58f, 0.20f, 0.02f, 0.06f},   // from Overcast
    {0.03f, 0.30f, 0.55f, 0.10f, 0.02f},   // from Rain
    {0.00f, 0.15f, 0.55f, 0.30f, 0.00f},   // from Storm
    {0.15f, 0.45f, 0.08f, 0.00f, 0.32f},   // from Fog
};

float targetWetness(Weather w) {
    switch (w) {
        case Weather::Rain: return 0.8f;
        case Weather::Storm: return 1.0f;
        case Weather::Fog: return 0.25f;
        default: return 0.0f;
    }
}

}  // namespace clock_detail

void WeatherSystem::update(float gameDt, Rng& rng, EventQueue& events) {
    // Roads wet in about ten game minutes and take roughly an hour to dry.
    const float target = clock_detail::targetWetness(current_);
    const float rate = target > wetness_ ? 1.0f / 600.0f : 1.0f / 3600.0f;
    wetness_ = approach(wetness_, target, rate * gameDt);

    untilNextRoll_ -= gameDt;
    while (untilNextRoll_ <= 0.0f) {
        untilNextRoll_ += 3600.0f;
        const float* row = clock_detail::kTransitions[static_cast<int>(current_)];
        float r = rng.nextFloat();
        Weather next = current_;
        for (int i = 0; i < clock_detail::kWeatherCount; ++i) {
            r -= row[i];
            if (r < 0.0f) {
                next = static_cast<Weather>(i);
                break;
            }
        }
        if (next != current_) force(next, events);
    }
}

void WeatherSystem::force(Weather w, EventQueue& events) {
    if (w == current_ || w == Weather::Count) return;
    current_ = w;
    events.push(EventType::WeatherChanged, static_cast<int>(w));
}

float WeatherSystem::visibility() const {
    switch (current_) {
        case Weather::Fog: return 0.35f;
        case Weather::Storm: return 0.55f;
        case Weather::Rain: return 0.75f;
        default: return 1.0f;
    }
}

float WeatherSystem::pedestrianFactor() const {
    switch (current_) {
        case Weather::Storm: return 0.25f;
        case Weather::Rain: return 0.5f;
        case Weather::Fog: return 0.8f;
        default: return 1.0f;
    }
}

float WeatherSystem::trafficFactor() const {
    switch (current_) {
        case Weather::Storm: return 0.7f;
        case Weather::Rain: return 0.9f;
        default: return 1.0f;
    }
}

Json WeatherSystem::save() const {
    Json j = Json::object();
    j.set("current", static_cast<int>(current_));
    j.set("wetness", wetness_);
    j.set("next", untilNextRoll_);
    return j;
}

void WeatherSystem::load(const Json& j) {
    const int w = j["current"].asInt(static_cast<int>(current_));
    if (w >= 0 && w < clock_detail::kWeatherCount) current_ = static_cast<Weather>(w);
    wetness_ = saturate(j["wetness"].asFloat(wetness_));
    untilNextRoll_ = clampf(j["next"].asFloat(untilNextRoll_), 1.0f, 3600.0f);
}

}  // namespace ut
