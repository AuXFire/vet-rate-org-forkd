#include <cmath>

#include "Sim/UtGeo.h"
#include "ut_test.h"

using namespace ut;

UT_TEST(geo, great_circle_distances_match_known_values) {
    // One degree of latitude is about 111.2 km on the mean sphere.
    UT_CHECK_NEAR(geo::distance(LatLon(0, 0), LatLon(1, 0)), 111195.0, 50.0);
    // London to New York: about 5,570 km.
    UT_CHECK_NEAR(geo::distance(LatLon(51.5074, -0.1278), LatLon(40.7128, -74.0060)), 5570000.0, 20000.0);
    // Pole to pole is half the circumference.
    UT_CHECK_NEAR(geo::distance(LatLon(90, 0), LatLon(-90, 0)), geo::kPiD * geo::kMeanRadius, 1.0);
    // Across the antimeridian is short, not the long way round.
    UT_CHECK(geo::distance(LatLon(0, 179.9), LatLon(0, -179.9)) < 25000.0);
    UT_CHECK_NEAR(geo::wrapLon(190.0), -170.0, 1e-9);
    UT_CHECK_NEAR(geo::wrapLon(-181.0), 179.0, 1e-9);
}

UT_TEST(geo, local_frame_axes_and_scale) {
    const LocalFrame f(LatLon(48.0, 11.0));
    double e, n;
    f.toLocal(LatLon(48.0, 11.0), e, n);
    UT_CHECK_NEAR(e, 0.0, 1e-6);
    UT_CHECK_NEAR(n, 0.0, 1e-6);
    f.toLocal(LatLon(48.01, 11.0), e, n);              // north: +0.01 degrees is about 1,112 m
    UT_CHECK_NEAR(e, 0.0, 0.01);
    UT_CHECK_NEAR(n, 1111.9, 1.0);
    f.toLocal(LatLon(48.0, 11.01), e, n);              // east: scaled by cos(latitude)
    UT_CHECK_NEAR(e, 111320.0 * 0.01 * std::cos(48.0 * geo::kPiD / 180.0), 1.5);
    UT_CHECK_NEAR(n, 0.0, 0.2);
    // Local distance agrees with the great-circle distance at city scale.
    const LatLon a(48.02, 11.03);
    f.toLocal(a, e, n);
    UT_CHECK_NEAR(std::sqrt(e * e + n * n), geo::distance(f.anchor(), a), 12.0);
}

UT_TEST(geo, round_trip_is_millimetre_exact_anywhere_on_earth) {
    const LatLon anchors[] = {LatLon(0, 0), LatLon(51.5, -0.12), LatLon(-33.87, 151.21), LatLon(69.65, 18.96),
                              LatLon(-54.8, -68.3), LatLon(35.68, 179.999), LatLon(21.3, -179.999), LatLon(84.0, 40.0)};
    for (const LatLon& anchor : anchors) {
        const LocalFrame f(anchor);
        for (const double east : {-8000.0, -250.5, 0.0, 1234.567, 9000.0}) {
            for (const double north : {-9000.0, -0.25, 0.0, 777.125, 8000.0}) {
                const LatLon p = f.toGeo(east, north);
                double e, n;
                f.toLocal(p, e, n);
                UT_CHECK_NEAR(e, east, 1e-3);
                UT_CHECK_NEAR(n, north, 1e-3);
                UT_CHECK(p.lon >= -180.0 && p.lon < 180.0);
            }
        }
    }
}

UT_TEST(geo, rebasing_between_nearby_frames_preserves_relative_positions) {
    // Two anchors 3 km apart: a pair of points 50 m apart must be 50 m apart in both frames.
    const LocalFrame a(LatLon(60.17, 24.94));
    const LocalFrame b(a.toGeo(3000.0, -500.0));
    const LatLon p = a.toGeo(1500.0, 200.0), q = a.toGeo(1530.0, 240.0);
    const Vec2 pa = a.toLocal(p), qa = a.toLocal(q), pb = b.toLocal(p), qb = b.toLocal(q);
    UT_CHECK_NEAR(distance(pa, qa), 50.0f, 1e-3);
    UT_CHECK_NEAR(distance(pb, qb), 50.0f, 2e-3);
    // And the offset between the frames is the anchor offset (to within the tiny rotation between them).
    UT_CHECK_NEAR(pa.x - pb.x, 3000.0f, 1.0);
    UT_CHECK_NEAR(pa.y - pb.y, -500.0f, 2.0);   // 3 km apart at 60 degrees north the axes differ by 0.05 degrees
}

UT_TEST(geo, tiles_follow_the_web_map_grid) {
    // Well-known tile: zoom 14 over central London.
    const TileKey london = geo::tileAt(LatLon(51.5074, -0.1278), 14);
    UT_CHECK_EQ(london.x, 8186);
    UT_CHECK_EQ(london.y, 5448);
    const TileKey origin = geo::tileAt(LatLon(0.0001, 0.0001), 1);
    UT_CHECK_EQ(origin.x, 1);
    UT_CHECK_EQ(origin.y, 0);
    UT_CHECK_EQ(geo::tileAt(LatLon(89.9, 0), 4).y, 0);          // beyond the grid: clamped
    UT_CHECK_EQ(geo::tileAt(LatLon(-89.9, 0), 4).y, 15);
    LatLon nw, se;
    geo::tileBounds(london, nw, se);
    UT_CHECK(nw.lat > 51.5074 && se.lat < 51.5074 && nw.lon < -0.1278 && se.lon > -0.1278);
    UT_CHECK(geo::tileAt(LatLon((nw.lat + se.lat) * 0.5, (nw.lon + se.lon) * 0.5), 14) == london);
    UT_CHECK((TileKey{14, 1, 2}.packed() != TileKey{14, 2, 1}.packed()));
}

UT_TEST(geo, tiles_around_a_point_cover_the_radius_and_wrap) {
    std::vector<TileKey> tiles;
    geo::tilesAround(LatLon(51.5074, -0.1278), 1500.0, 14, tiles);
    UT_CHECK(tiles.size() >= 4 && tiles.size() <= 16);
    bool hasCentre = false;
    for (const TileKey& k : tiles) hasCentre = hasCentre || k == geo::tileAt(LatLon(51.5074, -0.1278), 14);
    UT_CHECK(hasCentre);
    // Every point on the circle falls in one of the returned tiles.
    const LocalFrame f(LatLon(51.5074, -0.1278));
    for (int i = 0; i < 36; ++i) {
        const double a = i * geo::kPiD / 18.0;
        const TileKey k = geo::tileAt(f.toGeo(1500.0 * std::cos(a), 1500.0 * std::sin(a)), 14);
        bool found = false;
        for (const TileKey& t : tiles) found = found || t == k;
        UT_CHECK(found);
    }
    // Straddling the antimeridian: tiles from both edges of the grid.
    geo::tilesAround(LatLon(10.0, 179.999), 3000.0, 12, tiles);
    bool west = false, east = false;
    for (const TileKey& k : tiles) {
        west = west || k.x == 0;
        east = east || k.x == (1 << 12) - 1;
    }
    UT_CHECK(west && east);
}

UT_TEST(geo, frame_maps_match_the_exact_conversion) {
    const LocalFrame from(LatLon(64.13, -21.9));
    const LocalFrame to(from.toGeo(1400.0, 900.0));
    const FrameMap map(from, to, to.anchor());
    for (const Vec2 p : {Vec2(1400.0f, 900.0f), Vec2(1000.0f, 1200.0f), Vec2(1900.0f, 300.0f), Vec2(600.0f, 700.0f)}) {
        const Vec2 exact = to.toLocal(from.toGeo(p));
        UT_CHECK(distance(map.point(p), exact) < 0.003f);
        UT_CHECK(distance(map.inverse().point(map.point(p)), p) < 0.003f);
    }
    // Headings turn by the same angle as directions do.
    const Vec2 d = Vec2::fromAngle(0.7f);
    UT_CHECK_NEAR(map.direction(d).angle(), map.heading(0.7f), 1e-5);
    UT_CHECK(std::fabs(map.rotation()) > 1e-5f && std::fabs(map.rotation()) < 0.01f);
    // The identity map changes nothing.
    UT_CHECK(distance(FrameMap().point(Vec2(12.5f, -3.0f)), Vec2(12.5f, -3.0f)) < 1e-6f);
}
