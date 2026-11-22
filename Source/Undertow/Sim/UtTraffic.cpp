#include "Sim/UtTraffic.h"

#include <algorithm>
#include <cmath>

namespace ut {

void TrafficTuning::load(const Json& j) {
    maxAgents = j["maxAgents"].asInt(maxAgents);
    spawnMin = j["spawnMin"].asFloat(spawnMin);
    spawnMax = j["spawnMax"].asFloat(spawnMax);
    despawn = j["despawn"].asFloat(despawn);
    accel = j["accel"].asFloat(accel);
    brake = j["brake"].asFloat(brake);
    headway = j["headway"].asFloat(headway);
    minGap = j["minGap"].asFloat(minGap);
    signalCycle = j["signalCycle"].asFloat(signalCycle);
    archetypes = std::max(1, j["archetypes"].asInt(archetypes));
}

void TrafficSystem::setGraph(const RoadGraph* graph) {
    graph_ = graph;
    agents_.clear();
    byEdge_.assign(graph ? static_cast<size_t>(graph->edgeCount()) : 0U, std::vector<int>());
    touchedEdges_.clear();
    candidatesAge_ = 1e9f;
    warm_ = false;
}

void TrafficSystem::clear(EventQueue& events) {
    for (const TrafficAgent& a : agents_) events.push(EventType::TrafficRemoved, a.id);
    agents_.clear();
    warm_ = false;
}

void TrafficSystem::graphRebuilt(const std::vector<int>& oldToNew, EventQueue& events) {
    auto mapped = [&](int old) { return old >= 0 && old < static_cast<int>(oldToNew.size()) ? oldToNew[static_cast<size_t>(old)] : -1; };
    size_t kept = 0;
    for (TrafficAgent& a : agents_) {
        const int edge = mapped(a.edge);
        if (edge < 0) {
            events.push(EventType::TrafficRemoved, a.id);
            continue;
        }
        a.edge = edge;
        a.nextEdge = mapped(a.nextEdge);
        const RoadEdge& e = graph_->edge(edge);
        a.s = std::min(a.s, e.length);
        a.lane = std::min(a.lane, e.lanes - 1);
        a.pos = graph_->lanePosition(edge, a.s, a.lane);
        a.heading = e.dir.angle();
        agents_[kept++] = a;
    }
    agents_.resize(kept);
    byEdge_.assign(static_cast<size_t>(graph_->edgeCount()), std::vector<int>());
    touchedEdges_.clear();
    obstacles_.clear();
    spawnCandidates_.clear();
    candidatesAge_ = 1e9f;
}

void TrafficSystem::addObstacle(Vec2 pos, Vec2 velocity, float radius) {
    if (!graph_) return;
    float s = 0.0f, dist = 0.0f;
    const int ei = graph_->nearestEdge(pos, 12.0f + radius, &s, &dist);
    if (ei < 0) return;
    const RoadEdge& e = graph_->edge(ei);
    if (dist > static_cast<float>(e.lanes) * RoadGraph::kLaneWidth + radius) return;
    // Blocks the whole direction it sits on. Speed along the edge lets traffic follow a moving car instead
    // of treating it as a wall.
    obstacles_.push_back({ei, s, std::max(0.0f, velocity.dot(e.dir))});
}

void TrafficSystem::panicAt(Vec2 pos, float radius, float seconds) {
    const float r2 = radius * radius;
    for (TrafficAgent& a : agents_) {
        if (distanceSq(a.pos, pos) <= r2) a.panic = std::max(a.panic, seconds);
    }
}

bool TrafficSystem::signalGreen(int edgeIndex) const {
    const RoadEdge& e = graph_->edge(edgeIndex);
    if (!graph_->node(e.to).signal) return true;
    // Two phases: roads running mostly east-west get the first half of the cycle, north-south the second.
    // Offsetting by node index stops every light in the city changing at once.
    const bool eastWest = std::fabs(e.dir.x) >= std::fabs(e.dir.y);
    const float phase = std::fmod(time_ + static_cast<float>(e.to % 7) * 5.0f, tuning.signalCycle) / tuning.signalCycle;
    return eastWest ? phase < 0.5f : phase >= 0.5f;
}

void TrafficSystem::rebuildBuckets() {
    for (const int ei : touchedEdges_) byEdge_[static_cast<size_t>(ei)].clear();
    touchedEdges_.clear();
    for (int i = 0; i < count(); ++i) {
        std::vector<int>& bucket = byEdge_[static_cast<size_t>(agents_[static_cast<size_t>(i)].edge)];
        if (bucket.empty()) touchedEdges_.push_back(agents_[static_cast<size_t>(i)].edge);
        bucket.push_back(i);
    }
}

float TrafficSystem::leaderGap(const TrafficAgent& a, float& leaderSpeed) const {
    float gap = 1e9f;
    leaderSpeed = 0.0f;
    const RoadEdge& e = graph_->edge(a.edge);

    const float toEnd = e.length - a.s;
    const bool nearJunction = a.nextEdge >= 0 && toEnd < 60.0f;
    const int myNextLane = a.nextEdge >= 0 ? std::min(a.lane, graph_->edge(a.nextEdge).lanes - 1) : a.lane;

    // Cars ahead on this edge: in my lane, or in another lane that merges into mine at the junction.
    for (const int idx : byEdge_[static_cast<size_t>(a.edge)]) {
        const TrafficAgent& o = agents_[static_cast<size_t>(idx)];
        if (o.id == a.id) continue;
        const bool merging = nearJunction && o.lane != a.lane && o.nextEdge == a.nextEdge &&
                             std::min(o.lane, graph_->edge(a.nextEdge).lanes - 1) == myNextLane;
        if (o.lane != a.lane && !merging) continue;
        if (o.s < a.s || (o.s == a.s && o.id > a.id)) continue;   // behind me; exact ties go to the lower id
        const float g = o.s - o.length - a.s;
        if (g < gap) {
            gap = g;
            leaderSpeed = o.speed;
        }
    }
    // Cars on other roads into the same junction, bound for the same lane of the same exit: whoever is
    // closer to the junction goes first, the other treats it as a leader.
    if (nearJunction) {
        for (const int ie : graph_->node(e.to).in) {
            if (ie == a.edge) continue;
            for (const int idx : byEdge_[static_cast<size_t>(ie)]) {
                const TrafficAgent& o = agents_[static_cast<size_t>(idx)];
                if (o.nextEdge != a.nextEdge || std::min(o.lane, graph_->edge(a.nextEdge).lanes - 1) != myNextLane) continue;
                const float otherToEnd = graph_->edge(ie).length - o.s;
                if (otherToEnd > toEnd || (otherToEnd == toEnd && o.id > a.id)) continue;
                const float g = toEnd - otherToEnd - o.length;
                if (g < gap) {
                    gap = g;
                    leaderSpeed = o.speed;
                }
            }
        }
    }
    // The last car on the next edge, if we are close enough to the junction for it to matter.
    if (a.nextEdge >= 0 && toEnd < 80.0f) {
        for (const int idx : byEdge_[static_cast<size_t>(a.nextEdge)]) {
            const TrafficAgent& o = agents_[static_cast<size_t>(idx)];
            if (o.lane != myNextLane) continue;
            const float g = toEnd + o.s - o.length;
            if (g < gap) {
                gap = g;
                leaderSpeed = o.speed;
            }
        }
    }
    for (const Obstacle& ob : obstacles_) {
        float g = 1e9f;
        if (ob.edge == a.edge && ob.s > a.s) g = ob.s - a.s - 2.5f;
        else if (ob.edge == a.nextEdge) g = toEnd + ob.s - 2.5f;
        if (g < gap) {
            gap = g;
            leaderSpeed = ob.speed;
        }
    }
    // A red light is a stopped car at the stop line. Panicking drivers run it.
    if (a.panic <= 0.0f && !signalGreen(a.edge)) {
        const float g = toEnd - 3.0f;
        if (g > -2.0f && g < gap) {   // already past the line: keep going
            gap = g;
            leaderSpeed = 0.0f;
        }
    }
    return gap;
}

void TrafficSystem::spawn(Vec2 focus, int wanted, Rng& rng, EventQueue& events) {
    if (wanted <= 0) return;
    if (candidatesAge_ > 1.0f || distance(focus, candidatesFocus_) > 40.0f) {
        graph_->edgesNear(focus, tuning.spawnMax, spawnCandidates_);
        candidatesAge_ = 0.0f;
        candidatesFocus_ = focus;
    }
    if (spawnCandidates_.empty()) return;
    // On the first fill, populate the whole ring so the city is not empty for the opening seconds.
    const float minDist = warm_ ? tuning.spawnMin : 25.0f;
    int attempts = wanted * 6;
    while (wanted > 0 && attempts-- > 0) {
        const int ei = spawnCandidates_[static_cast<size_t>(rng.below(static_cast<int>(spawnCandidates_.size())))];
        const RoadEdge& e = graph_->edge(ei);
        if (e.length < 40.0f) continue;
        TrafficAgent a;
        a.edge = ei;
        a.lane = rng.below(e.lanes);
        a.s = rng.range(15.0f, e.length - 15.0f);   // clear of junctions, so nothing appears on top of a turning car
        a.pos = graph_->lanePosition(ei, a.s, a.lane);
        const float d = distance(a.pos, focus);
        if (d < minDist || d > tuning.spawnMax) continue;
        bool blocked = false;
        for (const int idx : byEdge_[static_cast<size_t>(ei)]) {
            const TrafficAgent& o = agents_[static_cast<size_t>(idx)];
            if (o.lane == a.lane && std::fabs(o.s - a.s) < 14.0f) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        a.id = nextId_++;
        a.nextEdge = graph_->randomNextEdge(ei, rng);
        a.desiredFactor = rng.range(0.82f, 1.12f);
        a.speed = e.speed * a.desiredFactor * 0.8f;
        a.archetype = static_cast<uint8_t>(rng.below(tuning.archetypes));
        a.length = a.archetype % 4 == 3 ? 6.4f : 4.6f;   // every fourth archetype is a van/truck
        a.heading = e.dir.angle();
        std::vector<int>& bucket = byEdge_[static_cast<size_t>(ei)];
        if (bucket.empty()) touchedEdges_.push_back(ei);
        bucket.push_back(count());
        agents_.push_back(a);
        events.push(EventType::TrafficSpawned, a.id, a.archetype, a.pos);
        --wanted;
    }
}

void TrafficSystem::update(float dt, Vec2 focus, float density, Rng& rng, EventQueue& events) {
    if (!graph_ || graph_->edgeCount() == 0) {
        obstacles_.clear();
        return;
    }
    time_ += dt;
    candidatesAge_ += dt;
    rebuildBuckets();

    // --- drive
    const float sqrtAB = 2.0f * std::sqrt(tuning.accel * tuning.brake);
    for (TrafficAgent& a : agents_) {
        const RoadEdge& e = graph_->edge(a.edge);
        const bool panicking = a.panic > 0.0f;
        a.panic = std::max(0.0f, a.panic - dt);
        float desired = e.speed * a.desiredFactor * (panicking ? 1.6f : 1.0f);

        // Slow for the turn onto the next edge: the sharper the turn, the lower the corner speed.
        const float toEnd = e.length - a.s;
        if (a.nextEdge >= 0) {
            const float turn = std::fabs(wrapAngle(graph_->edge(a.nextEdge).dir.angle() - e.dir.angle()));
            if (turn > 0.35f) {
                const float corner = lerpf(11.0f, 4.0f, saturate((turn - 0.35f) / 1.6f));
                desired = std::min(desired, std::sqrt(corner * corner + 2.0f * tuning.brake * std::max(0.0f, toEnd)));
            }
        }

        float leaderSpeed = 0.0f;
        const float gap = leaderGap(a, leaderSpeed);
        float accel = tuning.accel * (1.0f - std::pow(a.speed / std::max(desired, 0.1f), 4.0f));
        if (gap < 1e8f) {
            const float dv = a.speed - leaderSpeed;
            const float wantedGap = tuning.minGap + std::max(0.0f, a.speed * tuning.headway + a.speed * dv / sqrtAB);
            accel -= tuning.accel * sqr(wantedGap / std::max(gap, 0.1f));
        }
        accel = clampf(accel, -9.0f, tuning.accel * (panicking ? 1.8f : 1.0f));   // -9: emergency stop
        a.speed = std::max(0.0f, a.speed + accel * dt);
        a.s += a.speed * dt;
    }

    // --- move to the next edge, drop cars that left the ring
    for (int i = 0; i < count();) {
        TrafficAgent& a = agents_[static_cast<size_t>(i)];
        bool remove = false;
        while (a.s >= graph_->edge(a.edge).length) {
            if (a.nextEdge < 0) {
                remove = true;
                break;
            }
            a.s -= graph_->edge(a.edge).length;
            a.edge = a.nextEdge;
            a.lane = std::min(a.lane, graph_->edge(a.edge).lanes - 1);
            a.nextEdge = graph_->randomNextEdge(a.edge, rng);
        }
        if (!remove) {
            a.pos = graph_->lanePosition(a.edge, a.s, a.lane);
            a.heading = graph_->edge(a.edge).dir.angle();
            remove = distance(a.pos, focus) > tuning.despawn;
        }
        if (remove) {
            events.push(EventType::TrafficRemoved, a.id);
            agents_[static_cast<size_t>(i)] = agents_.back();
            agents_.pop_back();
        } else {
            ++i;
        }
    }

    // --- top up toward the budget
    rebuildBuckets();
    const int budget = static_cast<int>(static_cast<float>(tuning.maxAgents) * clampf(density, 0.0f, 2.0f));
    const int perTick = warm_ ? 3 : budget;
    spawn(focus, std::min(perTick, budget - count()), rng, events);
    warm_ = true;
    obstacles_.clear();
}

}  // namespace ut
