#include "Core/UtHeat.h"

#include <algorithm>
#include <cmath>

namespace ut {

const char* crimeName(Crime c) {
    switch (c) {
        case Crime::Trespass: return "trespass";
        case Crime::RecklessDriving: return "reckless_driving";
        case Crime::PropertyDamage: return "property_damage";
        case Crime::VehicleTheft: return "vehicle_theft";
        case Crime::Assault: return "assault";
        case Crime::Robbery: return "robbery";
        case Crime::Gunfire: return "gunfire";
        case Crime::VehicularAssault: return "vehicular_assault";
        case Crime::AssaultOnOfficer: return "assault_on_officer";
        case Crime::Count: break;
    }
    return "unknown";
}

void HeatTuning::load(const Json& j) {
    const Json& pts = j["points"];
    for (int i = 0; i < static_cast<int>(Crime::Count); ++i) {
        const Json& v = pts[crimeName(static_cast<Crime>(i))];
        if (v.isNumber()) points[i] = v.asFloat();
    }
    const Json& th = j["thresholds"];
    const Json& ev = j["evadeSeconds"];
    for (int i = 0; i <= kMaxHeatLevel; ++i) {
        if (th.at(i).isNumber()) thresholds[i] = th.at(i).asFloat();
        if (ev.at(i).isNumber()) evadeSeconds[i] = ev.at(i).asFloat();
    }
    reportDelay = j["reportDelay"].asFloat(reportDelay);
    searchRadiusStart = j["searchRadiusStart"].asFloat(searchRadiusStart);
    searchRadiusGrowth = j["searchRadiusGrowth"].asFloat(searchRadiusGrowth);
    searchRadiusMax = j["searchRadiusMax"].asFloat(searchRadiusMax);
    minorDecay = j["minorDecay"].asFloat(minorDecay);
}

int HeatSystem::levelForPoints(float pts) const {
    int lvl = 0;
    for (int i = 1; i <= kMaxHeatLevel; ++i) {
        if (pts >= tuning.thresholds[i]) lvl = i;
    }
    return lvl;
}

void HeatSystem::setState(HeatState s, EventQueue& events) {
    if (s == state_) return;
    state_ = s;
    events.push(EventType::HeatStateChanged, static_cast<int>(s));
}

void HeatSystem::reportCrime(Crime crime, Vec2 pos, bool seenByPolice, int civilianWitnesses, EventQueue& events) {
    if (crime == Crime::Count) return;
    if (seenByPolice) {
        addPoints(crime, pos, true, events);
    } else if (civilianWitnesses > 0) {
        Pending p;
        p.crime = crime;
        p.pos = pos;
        p.timer = tuning.reportDelay / std::sqrt(static_cast<float>(civilianWitnesses));
        pending_.push_back(p);
    }
    // No witnesses: nobody knows, nothing happens.
}

void HeatSystem::cancelPendingReports(Vec2 pos, float radius) {
    const float r2 = radius * radius;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending& p) { return distanceSq(p.pos, pos) <= r2; }),
                   pending_.end());
}

void HeatSystem::addPoints(Crime crime, Vec2 pos, bool seenByPolice, EventQueue& events) {
    const int before = level_;
    points_ += tuning.points[static_cast<int>(crime)];
    if (crime == Crime::AssaultOnOfficer) points_ = std::max(points_, tuning.thresholds[2]);
    points_ = std::min(points_, tuning.thresholds[kMaxHeatLevel] * 1.5f);
    level_ = levelForPoints(points_);
    events.push(EventType::CrimeReported, static_cast<int>(crime), 0, pos);
    if (level_ != before) events.push(EventType::HeatLevelChanged, level_, before, pos);
    if (level_ == 0) return;

    evadeTimer_ = 0.0f;   // any new report restarts the escape
    if (seenByPolice) {
        lastKnown_ = pos;
        searchRadius_ = tuning.searchRadiusStart;
        setState(HeatState::Pursuit, events);
    } else if (state_ != HeatState::Pursuit) {
        // A phoned-in report sends police to where it happened, not to where the player is now.
        lastKnown_ = pos;
        searchRadius_ = tuning.searchRadiusStart;
        setState(HeatState::Search, events);
    }
}

void HeatSystem::update(float dt, Vec2 playerPos, bool seenByPolice, EventQueue& events) {
    for (size_t i = 0; i < pending_.size();) {
        pending_[i].timer -= dt;
        if (pending_[i].timer <= 0.0f) {
            const Pending p = pending_[i];
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            addPoints(p.crime, p.pos, false, events);
        } else {
            ++i;
        }
    }

    if (level_ == 0) {
        points_ = std::max(0.0f, points_ - tuning.minorDecay * dt);
        setState(HeatState::Clean, events);
        return;
    }

    if (seenByPolice) {
        lastKnown_ = playerPos;
        searchRadius_ = tuning.searchRadiusStart;
        evadeTimer_ = 0.0f;
        setState(HeatState::Pursuit, events);
        return;
    }

    setState(HeatState::Search, events);
    searchRadius_ = std::min(tuning.searchRadiusMax, searchRadius_ + tuning.searchRadiusGrowth * dt);
    const bool outside = distance(playerPos, lastKnown_) > searchRadius_;
    evadeTimer_ += dt * (outside ? 2.0f : 1.0f);
    if (evadeTimer_ >= tuning.evadeSeconds[level_]) clear(events);
}

void HeatSystem::clear(EventQueue& events) {
    const int before = level_;
    points_ = 0.0f;
    level_ = 0;
    evadeTimer_ = 0.0f;
    searchRadius_ = 0.0f;
    pending_.clear();
    if (before != 0) {
        events.push(EventType::HeatLevelChanged, 0, before);
        events.push(EventType::HeatCleared);
    }
    setState(HeatState::Clean, events);
}

float HeatSystem::evadeProgress() const {
    if (level_ == 0 || state_ != HeatState::Search) return 0.0f;
    return saturate(evadeTimer_ / tuning.evadeSeconds[level_]);
}

Json HeatSystem::save() const {
    // Pending reports and pursuit state are deliberately not saved: loading a game starts the player clean of
    // anything in flight, but keeps the heat they had earned.
    Json j = Json::object();
    j.set("points", points_);
    return j;
}

void HeatSystem::load(const Json& j) {
    points_ = std::max(0.0f, j["points"].asFloat(0.0f));
    level_ = levelForPoints(points_);
    state_ = level_ > 0 ? HeatState::Search : HeatState::Clean;
    evadeTimer_ = 0.0f;
    searchRadius_ = level_ > 0 ? tuning.searchRadiusStart : 0.0f;
    pending_.clear();
}

}  // namespace ut
