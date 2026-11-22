// Simulation: the one object the game talks to. It owns every core system, advances them in a fixed order
// each tick, and queues events for the presentation layer.
//
//   Simulation sim(seed);
//   sim.loadCity(cityJson); sim.loadTuning(tuningJson); sim.loadMissions(missionsJson); sim.loadEconomy(econJson);
//   every frame:  sim.tick(dt, playerInput);  for (Event& e : sim.drainEvents()) { ...spawn actors, update HUD... }
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Sim/UtCity.h"
#include "Sim/UtClock.h"
#include "Sim/UtCrowd.h"
#include "Sim/UtDispatch.h"
#include "Sim/UtEconomy.h"
#include "Sim/UtEvents.h"
#include "Sim/UtHeat.h"
#include "Sim/UtMission.h"
#include "Sim/UtRng.h"
#include "Sim/UtRoadGraph.h"
#include "Sim/UtTraffic.h"

namespace ut {

// What the game reports about the player each tick.
struct PlayerInput {
    Vec2 pos;
    Vec2 velocity;
    bool inVehicle = false;
    std::string vehicleTag;
    // The game's own line-of-sight result. When `hasSightResult` is false (headless runs), the simulation
    // falls back to "any police unit within range".
    bool hasSightResult = false;
    bool seenByPolice = false;
};

class Simulation {
public:
    explicit Simulation(uint64_t seed = 1);

    // Each loader takes the text of one data file. They return false and set `error` on bad data.
    bool loadCity(const std::string& json, std::string* error = nullptr);
    bool loadTuning(const std::string& json, std::string* error = nullptr);
    bool loadMissions(const std::string& json, std::string* error = nullptr);
    bool loadEconomy(const std::string& json, std::string* error = nullptr);

    void tick(float dt, const PlayerInput& player);

    // Game -> simulation.
    void reportCrime(Crime crime, Vec2 pos, bool seenByPolice, int civilianWitnesses);
    void notifyMission(MissionNotice notice, const std::string& tag);
    bool startMission(const std::string& id) { return missions.start(id, events_); }
    bool buyItem(const std::string& id) { return economy.buyItem(id, events_); }
    bool buyProperty(const std::string& id) { return economy.buyProperty(id, events_); }

    // Pending events, oldest first. Call once per frame.
    std::vector<Event> drainEvents() { return events_.drain(); }
    const EventQueue& pendingEvents() const { return events_; }

    // Ambient population budgets for where the player is now.
    float trafficDensity() const { return trafficDensity_; }
    int pedestrianBudget() const { return pedestrianBudget_; }
    const District* currentDistrict() const { return district_; }

    // Save game as JSON text. load() leaves the simulation untouched and returns false if the text is not
    // a save this version understands.
    std::string save() const;
    bool load(const std::string& text, std::string* error = nullptr);
    Vec2 savedPlayerPosition() const { return lastPlayerPos_; }

    Rng rng;
    Clock clock;
    WeatherSystem weather;
    CityInfo city;
    RoadGraph roads;
    HeatSystem heat;
    TrafficSystem traffic;
    DispatchSystem dispatch;
    MissionSystem missions;
    Economy economy;
    Crowd crowd;

private:
    EventQueue events_;
    const District* district_ = nullptr;
    float trafficDensity_ = 0.5f;
    int pedestrianBudget_ = 0;
    Vec2 lastPlayerPos_;
    int lastDay_ = 1;
};

}  // namespace ut
