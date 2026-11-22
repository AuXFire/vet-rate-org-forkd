"""Checks on the generated world data: the files the game ships with must be internally consistent."""
import hashlib
import json
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "Tools"))
import build_data  # noqa: E402

DATA = os.path.join(ROOT, "Content", "Data")


def load(name):
    """The game's JSON reader allows // comments; strip them for Python's."""
    with open(os.path.join(DATA, name)) as f:
        return json.loads(re.sub(r"^\s*//.*$", "", f.read(), flags=re.M))


CITY = load("city.json")
NODES = CITY["roads"]["nodes"]
EDGES = CITY["roads"]["edges"]


def road_distance(pos):
    """Distance from a point to the nearest road centre line."""
    best = 1e9
    px, py = pos
    for a, b, *_ in EDGES:
        ax, ay, bx, by = NODES[a][0], NODES[a][1], NODES[b][0], NODES[b][1]
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        best = min(best, math.hypot(px - (ax + t * dx), py - (ay + t * dy)))
    return best


def test_committed_data_matches_the_generator():
    city = build_data.build_city()
    assert city == json.loads(json.dumps(CITY)), "run `python3 Tools/build_data.py` and commit the result"
    assert build_data.build_missions(city["pois"]) == load("missions.json")
    assert build_data.build_economy(city["pois"]) == load("economy.json")


def test_generator_is_deterministic_and_seed_sensitive():
    def digest(seed):
        return hashlib.sha256(json.dumps(build_data.build_city(seed), sort_keys=True).encode()).hexdigest()
    assert digest(7) == digest(7)
    assert digest(7) != digest(8)


def test_every_junction_is_reachable_from_every_other():
    adj = {}
    for a, b, *_ in EDGES:
        adj.setdefault(a, []).append(b)
        adj.setdefault(b, []).append(a)
    seen, stack = {0}, [0]
    while stack:
        for n in adj.get(stack.pop(), []):
            if n not in seen:
                seen.add(n)
                stack.append(n)
    assert len(seen) == len(NODES)


def test_roads_are_well_formed():
    x0, y0, x1, y1 = CITY["meta"]["bounds"]
    assert len({(n[0], n[1]) for n in NODES}) == len(NODES), "duplicate junctions"
    assert len({(min(a, b), max(a, b)) for a, b, *_ in EDGES}) == len(EDGES), "duplicate roads"
    for x, y, signal in NODES:
        assert x0 <= x <= x1 and y0 <= y <= y1 and signal in (0, 1)
    kinds = CITY["roads"]["kinds"]
    for a, b, lanes, speed, kind, oneway in EDGES:
        assert a != b and 0 <= a < len(NODES) and 0 <= b < len(NODES)
        assert 1 <= lanes <= 4 and 5 <= speed <= 35 and 0 <= kind < len(kinds) and oneway in (0, 1)
        length = math.dist(NODES[a][:2], NODES[b][:2])
        assert 15 <= length <= 450, "road %d-%d is %.0f m" % (a, b, length)


def test_no_road_crosses_water_except_the_bridges():
    rx0, rx1 = build_data.RIVER
    bridge = kinds_index = CITY["roads"]["kinds"].index("bridge")
    crossings = [(a, b) for a, b, _, _, kind, _ in EDGES if min(NODES[a][0], NODES[b][0]) < rx0 and max(NODES[a][0], NODES[b][0]) > rx1]
    assert len(crossings) == len(CITY["bridges"]) == 4
    assert all(kind == bridge for a, b, _, _, kind, _ in EDGES if (a, b) in crossings), kinds_index
    assert all(n[1] >= build_data.BAY_Y for n in NODES), "junction in the bay"


def test_buildings_stay_on_land_clear_of_roads_and_apart():
    rx0, rx1 = build_data.RIVER
    districts = CITY["districts"]
    for cx, cy, w, d, h, di in CITY["buildings"]:
        (ax, ay), _, (bx, by), _ = districts[di]["poly"]
        assert ax <= cx - w / 2 and cx + w / 2 <= bx and ay <= cy - d / 2 and cy + d / 2 <= by
        assert not (cx + w / 2 > rx0 and cx - w / 2 < rx1)
        assert w >= 5 and d >= 5 and 4 <= h <= 200
    # Sampled: building footprints must not sit on a carriageway.
    for cx, cy, w, d, _, _ in CITY["buildings"][::97]:
        assert road_distance((cx, cy)) > min(w, d) / 2 - 12.5, "building at %.0f,%.0f is in the road" % (cx, cy)


def test_districts_tile_without_overlap_and_downtown_is_tallest():
    rects = [(d["poly"][0][0], d["poly"][0][1], d["poly"][2][0], d["poly"][2][1]) for d in CITY["districts"]]
    for i, a in enumerate(rects):
        for b in rects[i + 1:]:
            assert a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1], "districts overlap"
    tallest = {}
    for *_, h, di in CITY["buildings"]:
        tallest[di] = max(tallest.get(di, 0), h)
    ids = [d["id"] for d in CITY["districts"]]
    assert max(tallest, key=tallest.get) == ids.index("heights")
    assert all(0 <= d["density"] <= 1 and 0 <= d["wealth"] <= 1 for d in CITY["districts"])


def test_locations_are_unique_on_land_and_beside_a_road():
    ids = [p["id"] for p in CITY["pois"]]
    assert len(ids) == len(set(ids))
    for p in CITY["pois"]:
        assert 3 <= road_distance(p["pos"]) <= 40, p["id"]
    types = {p["type"] for p in CITY["pois"]}
    assert {"safehouse", "police", "hospital", "shop", "garage", "mission", "property"} <= types


def test_missions_form_a_chain_the_player_can_finish():
    missions = load("missions.json")["missions"]
    ids = [m["id"] for m in missions]
    assert len(ids) == len(set(ids)) >= 6
    vehicle_tags = {t for v in load("vehicles.json")["vehicles"] for t in v["tags"]}
    known = {"goto", "enter_vehicle", "deliver", "collect", "eliminate", "lose_heat", "wait"}
    done, remaining = set(), list(missions)
    while remaining:                                   # every mission must eventually unlock
        ready = [m for m in remaining if set(m.get("requires", [])) <= done]
        assert ready, "missions locked forever: %s" % [m["id"] for m in remaining]
        for m in ready:
            done.add(m["id"])
            remaining.remove(m)
    for m in missions:
        assert m["title"] and m["intro"] and m["reward"]["cash"] > 0
        assert road_distance(m["start"]) <= 40
        for o in m["objectives"]:
            assert o["type"] in known and o["text"]
            if o["type"] in ("goto", "deliver"):
                assert road_distance(o["pos"]) <= 40 and o["radius"] >= 8
            if o["type"] in ("enter_vehicle", "deliver"):
                assert o["tag"] in vehicle_tags, "no vehicle is tagged %r" % o["tag"]
        for f in m.get("fail", []):
            assert f["type"] in ("vehicle_destroyed", "player_down")


def test_economy_and_tuning_are_sane():
    eco = load("economy.json")
    assert eco["startingCash"] >= 0
    item_ids = [i["id"] for i in eco["items"]]
    assert len(item_ids) == len(set(item_ids)) and all(i["price"] > 0 for i in eco["items"])
    poi_ids = {p["id"] for p in CITY["pois"]}
    for prop in eco["properties"]:
        assert prop["id"] in poi_ids
        assert 10 <= prop["price"] / prop["incomePerDay"] <= 40, "payback should take 10-40 in-game days"
    for w in load("weapons.json")["weapons"]:
        assert w["damage"] > 0 and w["range"] > 0 and w["noise"] > 0
        if "ammo" in w:
            assert w["ammo"] in item_ids
    tuning = load("tuning.json")
    heat = tuning["heat"]
    assert heat["thresholds"] == sorted(heat["thresholds"]) and len(heat["thresholds"]) == 6
    assert heat["evadeSeconds"] == sorted(heat["evadeSeconds"]) and len(heat["evadeSeconds"]) == 6
    assert tuning["dispatch"]["unitsPerLevel"] == sorted(tuning["dispatch"]["unitsPerLevel"])
    assert tuning["traffic"]["archetypes"] <= len(load("vehicles.json")["vehicles"])
    assert tuning["traffic"]["spawnMin"] < tuning["traffic"]["spawnMax"] < tuning["traffic"]["despawn"]
