// City metadata from city.json: districts (for density, wealth and naming) and points of interest.
// The road network lives in UtRoadGraph; building footprints are only used by the engine-side importer.
#pragma once

#include <string>
#include <vector>

#include "Core/UtJson.h"
#include "Core/UtMath.h"

namespace ut {

struct District {
    std::string id;
    std::string name;
    std::string kind;        // "industrial", "downtown", "oldtown", "residential", "hillside", "beach", "suburb", "canal"
    float density = 0.5f;    // 0..1: how busy the streets are
    float wealth = 0.5f;     // 0..1: drives vehicle and pedestrian variety
    std::vector<Vec2> poly;
};

struct Poi {
    std::string id;
    std::string type;        // "safehouse", "shop", "garage", "police", "hospital", "mission", "property"
    std::string name;
    Vec2 pos;
};

class CityInfo {
public:
    void load(const Json& city);

    const std::string& name() const { return name_; }
    const std::vector<District>& districts() const { return districts_; }
    const std::vector<Poi>& pois() const { return pois_; }
    // The district containing `pos`, or nullptr (water, outside the map).
    const District* districtAt(Vec2 pos) const;
    const Poi* findPoi(const std::string& id) const;
    // Nearest POI of a type, e.g. the hospital the player respawns at. nullptr if there is none.
    const Poi* nearestPoi(const std::string& type, Vec2 pos) const;

private:
    std::string name_;
    std::vector<District> districts_;
    std::vector<Poi> pois_;
};

}  // namespace ut
