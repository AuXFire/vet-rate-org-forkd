// Geodesy for a full-scale Earth.
//
// Positions on the planet are latitude/longitude in degrees on the WGS84 ellipsoid, held in doubles.
// Gameplay does not run in those coordinates: every system works in a LocalFrame, a flat east/north plane
// in meters tangent to the Earth at an anchor near the player, where 32-bit floats are accurate to well
// under a millimetre. As the player travels the host moves the anchor (a "rebase") and the simulation
// re-expresses its state in the new frame. Keep the anchor within a few kilometres of the player: 5 km
// away the tangent plane is already 2 m above the curved surface.
//
// World data is split into tiles on the standard web-map grid (the one OpenStreetMap and most tile
// services use), so road data for the whole planet can be stored and streamed by tile.
#pragma once

#include <cstdint>
#include <vector>

#include "Sim/UtMath.h"

namespace ut {

struct LatLon {
    double lat = 0.0;   // degrees, north positive
    double lon = 0.0;   // degrees, east positive

    constexpr LatLon() = default;
    constexpr LatLon(double inLat, double inLon) : lat(inLat), lon(inLon) {}
};

namespace geo {

constexpr double kPiD = 3.14159265358979323846;
constexpr double kEquatorRadius = 6378137.0;               // WGS84 semi-major axis, meters
constexpr double kFlattening = 1.0 / 298.257223563;
constexpr double kMeanRadius = 6371008.8;
constexpr double kMaxTileLat = 85.0511287798066;           // the web-map grid stops here

// Longitude wrapped to [-180, 180).
double wrapLon(double lon);
// Great-circle distance in meters on a sphere of mean radius (within about 0.3% of the true distance).
double distance(LatLon a, LatLon b);

}  // namespace geo

// A flat east/north plane tangent to the ellipsoid at `anchor`.
class LocalFrame {
public:
    LocalFrame() : LocalFrame(LatLon()) {}
    explicit LocalFrame(LatLon anchor);

    LatLon anchor() const { return anchor_; }
    // Meters east and north of the anchor. Exact to millimetres in doubles; the Vec2 version rounds to float.
    void toLocal(LatLon p, double& east, double& north) const;
    Vec2 toLocal(LatLon p) const;
    // Inverse of toLocal, for points on the surface.
    LatLon toGeo(double east, double north) const;
    LatLon toGeo(Vec2 local) const { return toGeo(static_cast<double>(local.x), static_cast<double>(local.y)); }

private:
    LatLon anchor_;
    double ox_ = 0.0, oy_ = 0.0, oz_ = 0.0;                 // anchor in Earth-centred coordinates
    double sinLat_ = 0.0, cosLat_ = 1.0, sinLon_ = 0.0, cosLon_ = 1.0;
    double metersPerDegLat_ = 111320.0, metersPerDegLon_ = 111320.0;
};

// Re-expresses local coordinates from one frame in another: a rotation and a shift, exact at `centre` and good
// to a millimetre within a kilometre or so of it. Used when the anchor moves (every position the simulation
// holds is passed through one of these) and by a host whose own origin differs from the simulation's.
class FrameMap {
public:
    FrameMap() = default;                                   // identity
    FrameMap(const LocalFrame& from, const LocalFrame& to, LatLon centre);

    Vec2 point(Vec2 p) const;
    // The same in doubles, for hosts whose own origin may be many kilometres from the player.
    void point(double east, double north, double& outEast, double& outNorth) const;
    Vec2 direction(Vec2 d) const;
    float heading(float h) const { return wrapAngle(h + rotation_); }
    // How far the new frame's axes are turned relative to the old one's, radians. Tiny except near the poles.
    float rotation() const { return rotation_; }
    FrameMap inverse() const;

private:
    double fromE_ = 0.0, fromN_ = 0.0, toE_ = 0.0, toN_ = 0.0;
    double cos_ = 1.0, sin_ = 0.0;
    float rotation_ = 0.0f;
};

// One tile of the web-map grid: at zoom z the world is 2^z by 2^z tiles, x eastward from the antimeridian,
// y southward from about 85 degrees north.
struct TileKey {
    int z = 0;
    int x = 0;
    int y = 0;

    bool operator==(const TileKey& o) const { return z == o.z && x == o.x && y == o.y; }
    bool operator!=(const TileKey& o) const { return !(*this == o); }
    bool operator<(const TileKey& o) const { return z != o.z ? z < o.z : (x != o.x ? x < o.x : y < o.y); }
    uint64_t packed() const {
        return (static_cast<uint64_t>(static_cast<uint32_t>(z)) << 58U) | (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 29U) |
               static_cast<uint64_t>(static_cast<uint32_t>(y));
    }
};

namespace geo {

// The tile containing a point. Latitudes beyond the grid are clamped to its top or bottom row.
TileKey tileAt(LatLon p, int zoom);
// North-west and south-east corners of a tile.
void tileBounds(TileKey key, LatLon& northWest, LatLon& southEast);
// Every tile that comes within `radiusMeters` of `center`, sorted. Wraps across the antimeridian.
void tilesAround(LatLon center, double radiusMeters, int zoom, std::vector<TileKey>& out);

}  // namespace geo
}  // namespace ut
