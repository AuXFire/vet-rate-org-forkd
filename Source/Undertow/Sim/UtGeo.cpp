#include "Sim/UtGeo.h"

#include <algorithm>
#include <cmath>

namespace ut {
namespace geo_detail {

constexpr double kDegToRad = geo::kPiD / 180.0;
constexpr double kE2 = geo::kFlattening * (2.0 - geo::kFlattening);   // first eccentricity squared

// Earth-centred, Earth-fixed coordinates of a point on the ellipsoid surface.
void toEcef(LatLon p, double& x, double& y, double& z) {
    const double lat = p.lat * kDegToRad, lon = p.lon * kDegToRad;
    const double sinLat = std::sin(lat), cosLat = std::cos(lat);
    const double n = geo::kEquatorRadius / std::sqrt(1.0 - kE2 * sinLat * sinLat);   // prime-vertical radius
    x = n * cosLat * std::cos(lon);
    y = n * cosLat * std::sin(lon);
    z = n * (1.0 - kE2) * sinLat;
}

double clampLat(double lat) { return std::min(std::max(lat, -geo::kMaxTileLat), geo::kMaxTileLat); }

// Fractional tile row for a latitude at a zoom level.
double tileYf(double lat, int zoom) {
    const double rad = clampLat(lat) * kDegToRad;
    return (1.0 - std::asinh(std::tan(rad)) / geo::kPiD) * 0.5 * std::ldexp(1.0, zoom);
}

}  // namespace geo_detail

namespace geo {

double wrapLon(double lon) {
    lon = std::fmod(lon + 180.0, 360.0);
    if (lon < 0.0) lon += 360.0;
    return lon - 180.0;
}

double distance(LatLon a, LatLon b) {
    const double lat1 = a.lat * geo_detail::kDegToRad, lat2 = b.lat * geo_detail::kDegToRad;
    const double dLat = lat2 - lat1, dLon = (b.lon - a.lon) * geo_detail::kDegToRad;
    const double s1 = std::sin(dLat * 0.5), s2 = std::sin(dLon * 0.5);
    const double h = s1 * s1 + std::cos(lat1) * std::cos(lat2) * s2 * s2;
    return 2.0 * kMeanRadius * std::asin(std::min(1.0, std::sqrt(h)));
}

TileKey tileAt(LatLon p, int zoom) {
    const int n = 1 << zoom;
    TileKey k;
    k.z = zoom;
    k.x = static_cast<int>(std::floor((wrapLon(p.lon) + 180.0) / 360.0 * static_cast<double>(n)));
    k.x = std::min(std::max(k.x, 0), n - 1);
    k.y = std::min(std::max(static_cast<int>(std::floor(geo_detail::tileYf(p.lat, zoom))), 0), n - 1);
    return k;
}

void tileBounds(TileKey key, LatLon& northWest, LatLon& southEast) {
    const double n = std::ldexp(1.0, key.z);
    auto latOf = [n](double y) { return std::atan(std::sinh(kPiD * (1.0 - 2.0 * y / n))) / geo_detail::kDegToRad; };
    northWest = LatLon(latOf(static_cast<double>(key.y)), static_cast<double>(key.x) / n * 360.0 - 180.0);
    southEast = LatLon(latOf(static_cast<double>(key.y + 1)), static_cast<double>(key.x + 1) / n * 360.0 - 180.0);
}

void tilesAround(LatLon center, double radiusMeters, int zoom, std::vector<TileKey>& out) {
    out.clear();
    const int n = 1 << zoom;
    const double dLat = radiusMeters / 111320.0;
    const double cosLat = std::max(0.01, std::cos(geo_detail::clampLat(center.lat) * geo_detail::kDegToRad));
    const double dLon = std::min(180.0, radiusMeters / (111320.0 * cosLat));
    const int y0 = std::max(0, static_cast<int>(std::floor(geo_detail::tileYf(center.lat + dLat, zoom))));
    const int y1 = std::min(n - 1, static_cast<int>(std::floor(geo_detail::tileYf(center.lat - dLat, zoom))));
    // Columns are counted from the centre outward so the range can cross the antimeridian.
    const double xf = (wrapLon(center.lon) + 180.0) / 360.0 * static_cast<double>(n);
    const double spanX = dLon / 360.0 * static_cast<double>(n);
    const int x0 = static_cast<int>(std::floor(xf - spanX)), x1 = static_cast<int>(std::floor(xf + spanX));
    const int columns = std::min(n, x1 - x0 + 1);
    for (int i = 0; i < columns; ++i) {
        const int x = ((x0 + i) % n + n) % n;
        for (int y = y0; y <= y1; ++y) {
            TileKey k;
            k.z = zoom;
            k.x = x;
            k.y = y;
            out.push_back(k);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

}  // namespace geo

LocalFrame::LocalFrame(LatLon anchor) : anchor_(anchor) {
    geo_detail::toEcef(anchor, ox_, oy_, oz_);
    const double lat = anchor.lat * geo_detail::kDegToRad, lon = anchor.lon * geo_detail::kDegToRad;
    sinLat_ = std::sin(lat);
    cosLat_ = std::cos(lat);
    sinLon_ = std::sin(lon);
    cosLon_ = std::cos(lon);
    // Meters per degree at the anchor, used for the first guess when inverting.
    const double w = std::sqrt(1.0 - geo_detail::kE2 * sinLat_ * sinLat_);
    const double meridional = geo::kEquatorRadius * (1.0 - geo_detail::kE2) / (w * w * w);
    const double primeVertical = geo::kEquatorRadius / w;
    metersPerDegLat_ = meridional * geo_detail::kDegToRad;
    metersPerDegLon_ = std::max(1e-6, primeVertical * cosLat_ * geo_detail::kDegToRad);
}

void LocalFrame::toLocal(LatLon p, double& east, double& north) const {
    double x, y, z;
    geo_detail::toEcef(p, x, y, z);
    const double dx = x - ox_, dy = y - oy_, dz = z - oz_;
    east = -sinLon_ * dx + cosLon_ * dy;
    north = -sinLat_ * cosLon_ * dx - sinLat_ * sinLon_ * dy + cosLat_ * dz;
}

Vec2 LocalFrame::toLocal(LatLon p) const {
    double e, n;
    toLocal(p, e, n);
    return Vec2(static_cast<float>(e), static_cast<float>(n));
}

LatLon LocalFrame::toGeo(double east, double north) const {
    // First guess from the local scale, then correct with the exact forward transform. Converges to
    // sub-millimetre in a few rounds for anything within tens of kilometres of the anchor.
    LatLon p(anchor_.lat + north / metersPerDegLat_, anchor_.lon + east / metersPerDegLon_);
    for (int i = 0; i < 6; ++i) {
        double e, n;
        toLocal(p, e, n);
        const double de = east - e, dn = north - n;
        if (std::fabs(de) < 1e-6 && std::fabs(dn) < 1e-6) break;
        const double cosHere = std::max(1e-6, std::cos(p.lat * geo_detail::kDegToRad));
        p.lat += dn / metersPerDegLat_;
        p.lon += de / (metersPerDegLon_ * cosHere / std::max(1e-6, cosLat_));
    }
    p.lat = std::min(std::max(p.lat, -90.0), 90.0);
    p.lon = geo::wrapLon(p.lon);
    return p;
}

FrameMap::FrameMap(const LocalFrame& from, const LocalFrame& to, LatLon centre) {
    from.toLocal(centre, fromE_, fromN_);
    to.toLocal(centre, toE_, toN_);
    // Where a point 100 m east of the centre (in the old frame) lands in the new one gives the rotation.
    double e = 0.0, n = 0.0;
    to.toLocal(from.toGeo(fromE_ + 100.0, fromN_), e, n);
    const double angle = std::atan2(n - toN_, e - toE_);
    cos_ = std::cos(angle);
    sin_ = std::sin(angle);
    rotation_ = static_cast<float>(angle);
}

void FrameMap::point(double east, double north, double& outEast, double& outNorth) const {
    const double dx = east - fromE_, dy = north - fromN_;
    outEast = toE_ + cos_ * dx - sin_ * dy;
    outNorth = toN_ + sin_ * dx + cos_ * dy;
}

Vec2 FrameMap::point(Vec2 p) const {
    double e = 0.0, n = 0.0;
    point(static_cast<double>(p.x), static_cast<double>(p.y), e, n);
    return Vec2(static_cast<float>(e), static_cast<float>(n));
}

Vec2 FrameMap::direction(Vec2 d) const {
    const double dx = static_cast<double>(d.x), dy = static_cast<double>(d.y);
    return Vec2(static_cast<float>(cos_ * dx - sin_ * dy), static_cast<float>(sin_ * dx + cos_ * dy));
}

FrameMap FrameMap::inverse() const {
    FrameMap m;
    m.fromE_ = toE_;
    m.fromN_ = toN_;
    m.toE_ = fromE_;
    m.toN_ = fromN_;
    m.cos_ = cos_;
    m.sin_ = -sin_;
    m.rotation_ = -rotation_;
    return m;
}

}  // namespace ut
