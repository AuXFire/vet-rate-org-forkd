// AI driver: turns "follow this path" into throttle, brake and steering for a physical vehicle.
// Pure pursuit steering (aim at a point a speed-dependent distance ahead on the path) plus a speed target
// that slows for upcoming curvature. Police and mission vehicles use it; ambient traffic does not need it.
//
// BicycleModel is a small kinematic car used by the headless tests and simulation so the driver can be
// verified without the engine. In Unreal the same controls feed the Chaos vehicle.
#pragma once

#include <vector>

#include "Core/UtMath.h"

namespace ut {

struct VehicleState {
    Vec2 pos;
    float heading = 0.0f;   // radians
    float speed = 0.0f;     // m/s, forward
};

struct DriveControls {
    float throttle = 0.0f;  // 0..1
    float brake = 0.0f;     // 0..1
    float steer = 0.0f;     // -1 (left) .. 1 (right)... see sign note in UtDriver.cpp
    bool handbrake = false;
};

struct DriverTuning {
    float wheelbase = 2.8f;
    float maxSteerAngle = 0.6f;       // radians at full lock
    float lookaheadMin = 6.0f;
    float lookaheadPerSpeed = 0.9f;   // seconds of travel
    float lookaheadMax = 30.0f;
    float lateralAccel = 7.0f;        // m/s^2 the driver is willing to corner at
    float brakeDecel = 7.0f;          // m/s^2 assumed available when planning to slow for a corner
    float arriveRadius = 6.0f;
};

class Driver {
public:
    DriverTuning tuning;

    void setPath(const std::vector<Vec2>& points);
    const std::vector<Vec2>& path() const { return path_; }
    bool hasPath() const { return path_.size() >= 2; }
    // True once the vehicle is within arriveRadius of the last point.
    bool arrived() const { return arrived_; }
    // Meters travelled along the path so far.
    float progress() const { return progress_; }

    // `cruiseSpeed`: the speed to hold on straights. Call every tick.
    DriveControls update(const VehicleState& state, float cruiseSpeed);

private:
    Vec2 pointAt(float distanceAlong) const;
    float curvatureAhead(float from, float span) const;

    std::vector<Vec2> path_;
    std::vector<float> cumulative_;   // distance along the path at each point
    float progress_ = 0.0f;
    bool arrived_ = false;
};

// Kinematic bicycle: enough physics to check the driver's decisions, not a handling model.
struct BicycleModel {
    VehicleState state;
    float wheelbase = 2.8f;
    float maxSteerAngle = 0.6f;
    float maxAccel = 4.5f;
    float maxBrake = 9.0f;
    float drag = 0.02f;

    void step(const DriveControls& controls, float dt);
};

}  // namespace ut
