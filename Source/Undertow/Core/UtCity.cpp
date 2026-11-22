#include "Core/UtCity.h"

namespace ut {
namespace city_detail {

bool pointInPolygon(const std::vector<Vec2>& poly, Vec2 p) {
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2 a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

}  // namespace city_detail

void CityInfo::load(const Json& city) {
    name_ = city["meta"]["name"].asString();
    districts_.clear();
    pois_.clear();
    const Json& ds = city["districts"];
    for (int i = 0; i < ds.size(); ++i) {
        const Json& j = ds.at(i);
        District d;
        d.id = j["id"].asString();
        d.name = j["name"].asString(d.id);
        d.kind = j["kind"].asString();
        d.density = saturate(j["density"].asFloat(0.5f));
        d.wealth = saturate(j["wealth"].asFloat(0.5f));
        const Json& poly = j["poly"];
        for (int k = 0; k < poly.size(); ++k) d.poly.push_back(Vec2(poly.at(k).at(0).asFloat(), poly.at(k).at(1).asFloat()));
        if (d.poly.size() >= 3) districts_.push_back(d);
    }
    const Json& ps = city["pois"];
    for (int i = 0; i < ps.size(); ++i) {
        const Json& j = ps.at(i);
        Poi p;
        p.id = j["id"].asString();
        p.type = j["type"].asString();
        p.name = j["name"].asString(p.id);
        p.pos = Vec2(j["pos"].at(0).asFloat(), j["pos"].at(1).asFloat());
        if (!p.id.empty()) pois_.push_back(p);
    }
}

const District* CityInfo::districtAt(Vec2 pos) const {
    for (const District& d : districts_) {
        if (city_detail::pointInPolygon(d.poly, pos)) return &d;
    }
    return nullptr;
}

const Poi* CityInfo::findPoi(const std::string& id) const {
    for (const Poi& p : pois_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const Poi* CityInfo::nearestPoi(const std::string& type, Vec2 pos) const {
    const Poi* best = nullptr;
    float bestD2 = 1e30f;
    for (const Poi& p : pois_) {
        if (p.type != type) continue;
        const float d2 = distanceSq(p.pos, pos);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = &p;
        }
    }
    return best;
}

}  // namespace ut
