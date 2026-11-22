// Money: the player's wallet, shop catalogue and income-producing properties.
#pragma once

#include <string>
#include <vector>

#include "Core/UtEvents.h"
#include "Core/UtJson.h"
#include "Core/UtMath.h"

namespace ut {

struct ShopItem {
    std::string id;
    std::string name;
    std::string category;   // "weapon", "ammo", "vehicle_mod", "clothing", "consumable"
    int price = 0;
};

struct Property {
    std::string id;
    std::string name;
    Vec2 pos;
    int price = 0;
    int incomePerDay = 0;
    bool owned = false;
};

class Economy {
public:
    // Reads {"startingCash": n, "items": [...], "properties": [...]}.
    void load(const Json& root);

    int cash() const { return cash_; }
    void earn(int amount, EventQueue& events);
    // Returns false (and changes nothing) if the player cannot afford it.
    bool spend(int amount, EventQueue& events);

    const std::vector<ShopItem>& items() const { return items_; }
    const std::vector<Property>& properties() const { return properties_; }
    const ShopItem* findItem(const std::string& id) const;
    bool buyItem(const std::string& id, EventQueue& events);
    bool buyProperty(const std::string& id, EventQueue& events);
    // Pays one day's income from every owned property. The simulation calls it when the day rolls over.
    void payDailyIncome(EventQueue& events);

    Json save() const;
    void load_state(const Json& j);

private:
    int cash_ = 0;
    std::vector<ShopItem> items_;
    std::vector<Property> properties_;
};

}  // namespace ut
