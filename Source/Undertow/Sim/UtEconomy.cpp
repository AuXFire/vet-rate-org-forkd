#include "Sim/UtEconomy.h"

#include <algorithm>

namespace ut {

void Economy::load(const Json& root) {
    cash_ = std::max(0, root["startingCash"].asInt(0));
    items_.clear();
    properties_.clear();
    const Json& items = root["items"];
    for (int i = 0; i < items.size(); ++i) {
        const Json& j = items.at(i);
        ShopItem it;
        it.id = j["id"].asString();
        it.name = j["name"].asString(it.id);
        it.category = j["category"].asString();
        it.price = std::max(0, j["price"].asInt(0));
        if (!it.id.empty()) items_.push_back(it);
    }
    const Json& props = root["properties"];
    for (int i = 0; i < props.size(); ++i) {
        const Json& j = props.at(i);
        Property p;
        p.id = j["id"].asString();
        p.name = j["name"].asString(p.id);
        p.pos = Vec2(j["pos"].at(0).asFloat(), j["pos"].at(1).asFloat());
        if (j["geo"].size() >= 2) {
            p.geo = LatLon(j["geo"].at(0).asDouble(), j["geo"].at(1).asDouble());
            p.hasGeo = true;
        }
        p.price = std::max(0, j["price"].asInt(0));
        p.incomePerDay = std::max(0, j["incomePerDay"].asInt(0));
        if (!p.id.empty()) properties_.push_back(p);
    }
}

void Economy::earn(int amount, EventQueue& events) {
    if (amount <= 0) return;
    const long long total = static_cast<long long>(cash_) + amount;
    cash_ = static_cast<int>(std::min<long long>(total, 2000000000LL));   // no overflow, however rich
    events.push(EventType::CashChanged, cash_, amount);
}

bool Economy::spend(int amount, EventQueue& events) {
    if (amount < 0 || amount > cash_) return false;
    cash_ -= amount;
    if (amount > 0) events.push(EventType::CashChanged, cash_, -amount);
    return true;
}

const ShopItem* Economy::findItem(const std::string& id) const {
    for (const ShopItem& it : items_) {
        if (it.id == id) return &it;
    }
    return nullptr;
}

bool Economy::buyItem(const std::string& id, EventQueue& events) {
    const ShopItem* it = findItem(id);
    return it != nullptr && spend(it->price, events);
}

bool Economy::buyProperty(const std::string& id, EventQueue& events) {
    for (Property& p : properties_) {
        if (p.id != id) continue;
        if (p.owned || !spend(p.price, events)) return false;
        p.owned = true;
        return true;
    }
    return false;
}

void Economy::payDailyIncome(EventQueue& events) {
    int total = 0;
    for (const Property& p : properties_) total += p.owned ? p.incomePerDay : 0;
    if (total <= 0) return;
    events.push(EventType::PropertyIncome, total);
    earn(total, events);
}

Json Economy::save() const {
    Json j = Json::object();
    j.set("cash", cash_);
    Json owned = Json::array();
    for (const Property& p : properties_) {
        if (p.owned) owned.push(p.id);
    }
    j.set("owned", owned);
    return j;
}

void Economy::load_state(const Json& j) {
    cash_ = std::max(0, j["cash"].asInt(cash_));
    const Json& owned = j["owned"];
    for (Property& p : properties_) {
        p.owned = false;
        for (int i = 0; i < owned.size(); ++i) p.owned = p.owned || owned.at(i).asString() == p.id;
    }
}

void Economy::georeference(const LocalFrame& dataFrame) {
    for (Property& p : properties_) {
        if (p.hasGeo) continue;
        p.geo = dataFrame.toGeo(p.pos);
        p.hasGeo = true;
    }
}

void Economy::relocalize(const LocalFrame& frame) {
    for (Property& p : properties_) {
        if (p.hasGeo) p.pos = frame.toLocal(p.geo);
    }
}

}  // namespace ut
