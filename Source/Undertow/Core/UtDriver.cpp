#include "Core/UtDriver.h"

#include <algorithm>
#include <cmath>

namespace ut {

// Steering sign: positive steer turns toward increasing heading (counter-clockwise in the sim's x-east,
// y-north plane). The Unreal layer flips or keeps the sign to match its vehicle input.

void Driver::setPath(const std::vector<Vec2>& points) {
    path_.clear();
    cumulative_.clear();
    for (const Vec2& p : points) {
        if (!path_.empty() && distanceSq(path_.back(), p) < 0.01f) continue;   // drop duplicates
        cumulative_.push_back(path_.empty() ? 0.0f : cumulative_.back() + distance(path_.back(), p));
        path_.push_back(p);
    }
    progress_ = 0.0f;
    arrived_ = path_.size() < 2;
}

Vec2 Driver::pointAt(float distanceAlong) const {
    if (path_.empty()) return Vec2();
    if (distanceAlong <= 0.0f) return path_.front();
    if (distanceAlong >= cumulative_.back()) return path_.back();
    const auto it = std::upper_bound(cumulative_.begin(), cumulative_.end(), distanceAlong);
    const size_t i = static_cast<size_t>(it - cumulative_.begin());
    const float seg = cumulative_[i] - cumulative_[i - 1];
    return lerp(path_[i - 1], path_[i], seg > 1e-6f ? (distanceAlong - cumulative_[i - 1]) / seg : 0.0f);
}

float Driver::curvatureAhead(float from, float span) const {
    // Largest direction change per meter over the next `span` meters, sampled every few meters.
    float worst = 0.0f;
    const float step = 4.0f;
    Vec2 a = pointAt(from);
    Vec2 b = pointAt(from + step);
    for (float d = from + step; d < from + span; d += step) {
        const Vec2 c = pointAt(d + step);
        const Vec2 ab = b - a, bc = c - b;
        if (ab.lengthSq() > 1e-4f && bc.lengthSq() > 1e-4f) {
            const float turn = std::fabs(wrapAngle(bc.angle() - ab.angle()));
            worst = std::max(worst, turn / step);
        }
        a = b;
        b = c;
    }
    return worst;
}

DriveControls Driver::update(const VehicleState& state, float cruiseSpeed) {
    DriveControls out;
    if (!hasPath() || arrived_) {
        out.brake = 1.0f;
        return out;
    }

    // Advance progress to the closest point on the path, searching only forward from the last known spot so
    // a path that crosses itself cannot make the car jump to the wrong pass.
    {
        float best = progress_;
        float bestD2 = distanceSq(pointAt(progress_), state.pos);
        const float limit = std::min(cumulative_.back(), progress_ + 40.0f);
        for (float d = progress_; d <= limit; d += 1.0f) {
            const float d2 = distanceSq(pointAt(d), state.pos);
            if (d2 < bestD2) {
                bestD2 = d2;
                best = d;
            }
        }
        progress_ = best;
    }

    const float remaining = cumulative_.back() - progress_;
    if (distance(state.pos, path_.back()) <= tuning.arriveRadius || remaining <= 0.5f) {
        arrived_ = true;
        out.brake = 1.0f;
        return out;
    }

    // --- steering: pure pursuit
    const float lookahead = clampf(tuning.lookaheadMin + state.speed * tuning.lookaheadPerSpeed, tuning.lookaheadMin, tuning.lookaheadMax);
    const Vec2 target = pointAt(progress_ + lookahead);
    const Vec2 toTarget = target - state.pos;
    const float alpha = wrapAngle(toTarget.angle() - state.heading);
    const float dist = std::max(toTarget.length(), 1.0f);
    const float curvature = 2.0f * std::sin(alpha) / dist;
    out.steer = clampf(std::atan(tuning.wheelbase * curvature) / tuning.maxSteerAngle, -1.0f, 1.0f);

    // --- speed: cruise on straights, slow so the sharpest bend within braking distance is taken at a
    // lateral acceleration the car can hold, and stop at the end of the path.
    const float brakingDistance = sqr(state.speed) / (2.0f * tuning.brakeDecel) + lookahead;
    const float kappa = curvatureAhead(progress_, brakingDistance);
    float targetSpeed = cruiseSpeed;
    if (kappa > 1e-4f) targetSpeed = std::min(targetSpeed, std::sqrt(tuning.lateralAccel / kappa));
    targetSpeed = std::min(targetSpeed, std::sqrt(2.0f * tuning.brakeDecel * std::max(0.0f, remaining - tuning.arriveRadius * 0.5f)) + 1.0f);
    // Facing the wrong way: crawl and turn rather than accelerate into a wall.
    if (std::fabs(alpha) > 1.2f) targetSpeed = std::min(targetSpeed, 5.0f);

    const float error = targetSpeed - state.speed;
    if (error >= 0.0f) {
        out.throttle = saturate(error * 0.5f + 0.1f);
    } else {
        out.brake = saturate(-error * 0.35f);
    }
    return out;
}

void BicycleModel::step(const DriveControls& controls, float dt) {
    float accel = controls.throttle * maxAccel - controls.brake * maxBrake - drag * state.speed;
    if (controls.handbrake) accel -= maxBrake;
    state.speed = std::max(0.0f, state.speed + accel * dt);
    const float steerAngle = clampf(controls.steer, -1.0f, 1.0f) * maxSteerAngle;
    state.heading = wrapAngle(state.heading + state.speed / wheelbase * std::tan(steerAngle) * dt);
    state.pos += Vec2::fromAngle(state.heading) * (state.speed * dt);
}

}  // namespace ut
