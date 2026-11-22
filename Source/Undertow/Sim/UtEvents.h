// Events the simulation emits for the presentation layer (HUD, audio, actor spawning). The core never calls
// into the engine: it appends events here and the Unreal layer drains them once per frame.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Sim/UtMath.h"

namespace ut {

enum class EventType : uint8_t {
    HeatLevelChanged,   // a = new level, b = old level
    HeatStateChanged,   // a = new HeatState
    HeatCleared,
    CrimeReported,      // a = Crime, pos = where
    TrafficSpawned,     // a = agent id
    TrafficRemoved,     // a = agent id
    UnitSpawned,        // a = police unit id, pos = where
    UnitRemoved,        // a = police unit id
    MissionStarted,     // text = mission id
    ObjectiveChanged,   // a = objective index, text = objective text
    MissionCompleted,   // a = cash reward, text = mission id
    MissionFailed,      // text = reason
    CashChanged,        // a = new balance, b = delta
    PropertyIncome,     // a = amount paid out
    WeatherChanged,     // a = new Weather
    DayStarted,         // a = day number
    RoadsChanged,       // the road graph was rebuilt: a = junction count. Cached routes and edge indices are stale
};

struct Event {
    EventType type = EventType::HeatCleared;
    int a = 0;
    int b = 0;
    Vec2 pos;
    std::string text;
};

class EventQueue {
public:
    void push(EventType type, int a = 0, int b = 0, Vec2 pos = Vec2(), std::string text = std::string()) {
        Event e;
        e.type = type;
        e.a = a;
        e.b = b;
        e.pos = pos;
        e.text = std::move(text);
        events_.push_back(std::move(e));
    }
    const std::vector<Event>& events() const { return events_; }
    // Hands the pending events to the caller and leaves the queue empty.
    std::vector<Event> drain() {
        std::vector<Event> out;
        out.swap(events_);
        return out;
    }
    int count(EventType type) const {
        int n = 0;
        for (const Event& e : events_) n += e.type == type ? 1 : 0;
        return n;
    }

private:
    std::vector<Event> events_;
};

}  // namespace ut
