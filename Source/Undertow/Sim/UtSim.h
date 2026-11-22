// Simulation: the one object the game talks to. It owns every core system, advances them in a fixed order
// each tick, and queues events for the presentation layer.
//
//   Simulation sim(seed);
//   sim.loadCity(cityJson); sim.loadTuning(tuningJson); sim.loadMissions(missionsJson); sim.loadEconomy(econJson);
//   every frame:  sim.tick(dt, playerInput);  for (Event& e : sim.drainEvents()) { ...spawn actors, update HUD... }
//
// Full-scale Earth. When the city data carries a real-world anchor, or enableWorld() is called, the simulation
// runs in "world mode": positions passed in and out are still local meters (x east, y north), but local to an
// anchor on the globe that the host moves as the player travels.
//   every frame:  for (TileKey k : sim.world.missing()) sim.provideTile(k, textOfTileFile(k));   // "" if none
//                 if (sim.wantsRebase(playerPos)) { FrameMap m = sim.setAnchor(sim.frame().toGeo(playerPos));
//                                                   playerPos = m.point(playerPos); ...move the host's origin... }
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
#include "Sim/UtWorld.h"

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

    // --- Full-scale Earth
    // Starts world mode at `anchor` without a city file (a pure streamed world). loadCity() calls this itself
    // when the city has a "meta.anchor": [latitude, longitude].
    void enableWorld(LatLon anchor);
    bool worldEnabled() const { return world_; }
    // The current local frame: its anchor is where local (0, 0) sits on the globe.
    const LocalFrame& frame() const { return frame_; }
    LatLon playerGeo() const { return frame_.toGeo(lastPlayerPos_); }
    // True once the player is far enough from the anchor that the host should move it.
    bool wantsRebase(Vec2 playerPos) const { return world_ && playerPos.lengthSq() > rebaseDistance * rebaseDistance; }
    // Moves the anchor. Everything the simulation holds is re-expressed in the new frame; the returned map
    // does the same for positions the host holds.
    FrameMap setAnchor(LatLon anchor);
    // Hands over the text of a road tile file the world asked for. Empty text means there is no data for that
    // tile. Returns false (and treats the tile as empty) if the text is malformed.
    bool provideTile(TileKey key, const std::string& json, std::string* error = nullptr);
    float rebaseDistance = 1500.0f;

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
    // How many times the road graph has been rebuilt; changes whenever cached edge or node indices go stale.
    int roadsVersion() const { return roadsVersion_; }

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
    WorldRoads world;

private:
    void rebuildRoads();
    void relocalizeContent();
    float ambientDensity(Vec2 pos);

    EventQueue events_;
    const District* district_ = nullptr;
    float trafficDensity_ = 0.5f;
    int pedestrianBudget_ = 0;
    Vec2 lastPlayerPos_;
    int lastDay_ = 1;
    // World mode.
    bool world_ = false;
    LocalFrame frame_;          // current anchor
    LocalFrame dataFrame_;      // the frame the city, mission and economy files were written in
    Vec2 lastFocus_;
    bool hasFocus_ = false;
    float dirtyAge_ = 0.0f;
    float densityAge_ = 1e9f;
    float roadDensity_ = 0.3f;
    int roadsVersion_ = 0;
    std::vector<int> scratchNodes_;
};

}  // namespace ut
