#!/usr/bin/env python3
"""Generates the game's world data for Port Meridian.

    python3 Tools/build_data.py            # writes Content/Data/{city,missions,economy}.json and Docs/map.svg

Everything is derived from the seed, so the same seed always produces the same city. Coordinates are meters,
x east, y north. The output is a blockout: street layout, building masses and gameplay locations to build
the real environment on top of in Unreal, not final art.
"""
import html
import json
import math
import os
import random
import sys

SEED = 7
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------- city plan
# (id, name, kind, x0, y0, x1, y1, density, wealth)
DISTRICTS = [
    ("ironworks", "Ironworks", "industrial", 300, 650, 2900, 1500, 0.35, 0.20),
    ("narrows", "The Narrows", "canal", 300, 1500, 1500, 2900, 0.65, 0.30),
    ("lantern", "Lantern Quarter", "oldtown", 1500, 1500, 2900, 2900, 0.90, 0.55),
    ("saltgrass", "Saltgrass", "residential", 300, 2900, 2900, 4000, 0.50, 0.45),
    ("northgate", "Northgate", "suburb", 300, 4000, 2900, 4900, 0.40, 0.60),
    ("breakwater", "Breakwater", "beach", 3300, 650, 6100, 1500, 0.70, 0.65),
    ("heights", "Meridian Heights", "downtown", 3300, 1500, 4900, 3200, 1.00, 0.80),
    ("halfmoon", "Halfmoon", "residential", 3300, 3200, 4900, 4900, 0.55, 0.50),
    ("cinder", "Cinder Hill", "hillside", 4900, 1500, 6100, 4900, 0.30, 0.95),
]

# kind -> (street spacing m, position jitter m, share of streets removed, (min, max) building height m,
#          share of blocks left empty, speed limit m/s)
KINDS = {
    "industrial": (170, 6, 0.10, (7, 16), 0.25, 13.9),
    "canal": (95, 8, 0.12, (7, 15), 0.10, 11.1),
    "oldtown": (75, 12, 0.18, (10, 22), 0.06, 8.3),
    "residential": (115, 6, 0.08, (6, 12), 0.12, 11.1),
    "suburb": (140, 10, 0.15, (5, 9), 0.22, 13.9),
    "beach": (120, 5, 0.08, (8, 30), 0.15, 11.1),
    "downtown": (95, 2, 0.03, (30, 190), 0.07, 11.1),
    "hillside": (135, 22, 0.28, (6, 11), 0.35, 11.1),
}

RIVER = (3000, 3200)          # Wren River: water between these x values
BAY_Y = 560                   # Meridian Bay: water south of this
BRIDGES = [("Harbor Bridge", 1075), ("Lantern Bridge", 2200), ("Wren Bridge", 3550), ("North Span", 4450)]
BOUNDS = (0, 0, 6400, 5200)
ROAD_KINDS = ["alley", "street", "arterial", "highway", "bridge"]
RING = {("v", 300), ("v", 6100), ("h", 4900)}   # outer ring road: faster and wider than the other arterials


def snap(v):
    return round(v, 1)


class Builder:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.nodes = []            # [x, y]
        self.index = {}            # (x, y) -> node id
        self.lines = {}            # ("v", x) | ("h", y) -> set of node ids on that district boundary
        self.edges = {}            # (a, b) with a < b -> [lanes, speed, kind]
        self.buildings = []

    def node(self, x, y):
        key = (snap(x), snap(y))
        if key not in self.index:
            self.index[key] = len(self.nodes)
            self.nodes.append([key[0], key[1]])
        return self.index[key]

    def road(self, a, b, lanes, speed, kind):
        if a == b:
            return
        key = (min(a, b), max(a, b))
        old = self.edges.get(key)
        if old is None or ROAD_KINDS.index(kind) > ROAD_KINDS.index(old[2]):
            self.edges[key] = [lanes, speed, kind]

    # ---------------------------------------------------------------- one district
    def district(self, idx, d):
        _, _, kind, x0, y0, x1, y1, _, _ = d
        spacing, jitter, removed, heights, empty, speed = KINDS[kind]
        nx = max(2, round((x1 - x0) / spacing))
        ny = max(2, round((y1 - y0) / spacing))
        xs = [x0 + (x1 - x0) * i / nx for i in range(nx + 1)]
        ys = [y0 + (y1 - y0) * j / ny for j in range(ny + 1)]
        grid = {}
        for i, x in enumerate(xs):
            for j, y in enumerate(ys):
                on_v = i in (0, nx)
                on_h = j in (0, ny)
                jx = 0 if on_v else self.rng.uniform(-jitter, jitter)
                jy = 0 if on_h else self.rng.uniform(-jitter, jitter)
                n = self.node(x + jx, y + jy)
                grid[i, j] = n
                if on_v:
                    self.lines.setdefault(("v", snap(x)), set()).add(n)
                if on_h:
                    self.lines.setdefault(("h", snap(y)), set()).add(n)

        # Streets: every grid link strictly inside the district (boundary links are added later as arterials).
        links = []
        for i in range(nx + 1):
            for j in range(ny + 1):
                if i < nx and 0 < j < ny:
                    links.append((grid[i, j], grid[i + 1, j]))
                if j < ny and 0 < i < nx:
                    links.append((grid[i, j], grid[i, j + 1]))
        self.rng.shuffle(links)
        # Remove a share of streets for irregular blocks, but never disconnect anything: a removed street
        # is put back if its two ends would otherwise not be joined.
        parent = {}

        def find(a):
            while parent.setdefault(a, a) != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        # The boundary is one connected loop.
        ring = [grid[i, 0] for i in range(nx + 1)] + [grid[nx, j] for j in range(ny + 1)] + \
               [grid[i, ny] for i in range(nx + 1)] + [grid[0, j] for j in range(ny + 1)]
        for n in ring:
            parent[find(n)] = find(ring[0])
        dropped = []
        lanes = 2 if kind == "downtown" else 1
        for a, b in links:
            if self.rng.random() < removed:
                dropped.append((a, b))
            else:
                self.road(a, b, lanes, speed, "street")
                parent[find(a)] = find(b)
        for a, b in dropped:
            if find(a) != find(b):
                self.road(a, b, lanes, speed, "street")
                parent[find(a)] = find(b)

        # Buildings: split each block into lots set back from the street.
        setback = 11.0 if kind != "oldtown" else 8.0
        for i in range(nx):
            for j in range(ny):
                if self.rng.random() < empty:
                    continue                       # park, plaza or car park
                bx0, bx1 = xs[i] + setback, xs[i + 1] - setback
                by0, by1 = ys[j] + setback, ys[j + 1] - setback
                if bx1 - bx0 < 12 or by1 - by0 < 12:
                    continue
                if kind == "industrial":
                    cols, rows = 1, self.rng.choice([1, 1, 2])
                elif kind in ("suburb", "hillside", "residential"):
                    cols, rows = max(1, int((bx1 - bx0) // 26)), 2
                else:
                    cols, rows = max(1, int((bx1 - bx0) // 34)), max(1, int((by1 - by0) // 34))
                w, dpt = (bx1 - bx0) / cols, (by1 - by0) / rows
                gap = 6.0 if kind in ("suburb", "hillside", "residential") else 1.5
                for c in range(cols):
                    for r in range(rows):
                        if kind in ("suburb", "hillside") and self.rng.random() < 0.15:
                            continue
                        h = self.rng.uniform(*heights)
                        if kind == "downtown":      # a few towers, mostly mid-rise
                            h = heights[0] + (heights[1] - heights[0]) * self.rng.random() ** 3
                        self.buildings.append([snap(bx0 + (c + 0.5) * w), snap(by0 + (r + 0.5) * dpt),
                                               snap(w - gap), snap(dpt - gap), snap(h), idx])

    # ---------------------------------------------------------------- arterials, bridges, signals
    def stitch(self):
        rects = [(d[3], d[4], d[5], d[6]) for d in DISTRICTS]

        def on_boundary(axis, value, mid):
            for x0, y0, x1, y1 in rects:
                if axis == "v" and value in (x0, x1) and y0 <= mid <= y1:
                    return True
                if axis == "h" and value in (y0, y1) and x0 <= mid <= x1:
                    return True
            return False

        for west_x, east_x in [(2900, 3300)]:
            for _, by in BRIDGES:
                a, b = self.node(west_x, by), self.node(east_x, by)
                self.lines[("v", float(west_x))].add(a)
                self.lines[("v", float(east_x))].add(b)
                self.road(a, b, 2, 16.7, "bridge")
        # Neighbouring districts put junctions on their shared boundary at their own spacing, so two can land
        # a couple of meters apart. Merge any pair closer than 25 m into one junction (corners win).
        alias = {}

        def res(n):
            while n in alias:
                n = alias[n]
            return n

        membership = {}
        for ids in self.lines.values():
            for n in ids:
                membership[n] = membership.get(n, 0) + 1
        for (axis, value), ids in sorted(self.lines.items()):
            coord = 1 if axis == "v" else 0
            kept = []
            for n in sorted({res(i) for i in ids}, key=lambda i: (self.nodes[i][coord], i)):
                if kept and self.nodes[n][coord] - self.nodes[kept[-1]][coord] < 25:
                    if membership.get(n, 0) > membership.get(kept[-1], 0):
                        alias[kept[-1]] = n
                        kept[-1] = n
                    else:
                        alias[n] = kept[-1]
                else:
                    kept.append(n)
        merged = {}
        for (a, b), spec in self.edges.items():
            a, b = res(a), res(b)
            if a != b:
                key = (min(a, b), max(a, b))
                if key not in merged or ROAD_KINDS.index(spec[2]) > ROAD_KINDS.index(merged[key][2]):
                    merged[key] = spec
        self.edges = merged
        self.lines = {k: {res(i) for i in ids} for k, ids in self.lines.items()}

        for (axis, value), ids in sorted(self.lines.items()):
            coord = 1 if axis == "v" else 0
            ordered = sorted(ids, key=lambda n: self.nodes[n][coord])
            ring = (axis, value) in {(a, float(v)) for a, v in RING}
            for a, b in zip(ordered, ordered[1:]):
                mid = (self.nodes[a][coord] + self.nodes[b][coord]) / 2
                if on_boundary(axis, value, mid):
                    self.road(a, b, 3 if ring else 2, 22.2 if ring else 16.7, "highway" if ring else "arterial")

    def finish(self):
        degree = {}
        big = {}
        for (a, b), (_, _, kind) in self.edges.items():
            for n in (a, b):
                degree[n] = degree.get(n, 0) + 1
                if kind in ("arterial", "highway", "bridge"):
                    big[n] = big.get(n, 0) + 1
        nodes, remap = [], {}
        for i, (x, y) in enumerate(self.nodes):
            if i not in degree:
                continue                  # merged away above
            # Lights where two big roads cross, and at every other busy junction along them.
            signal = big.get(i, 0) >= 3 or (big.get(i, 0) == 2 and degree.get(i, 0) >= 4 and i % 2 == 0)
            remap[i] = len(nodes)
            nodes.append([x, y, 1 if signal else 0])
        edges = sorted([remap[a], remap[b], lanes, speed, ROAD_KINDS.index(kind), 0] for (a, b), (lanes, speed, kind) in self.edges.items())
        return nodes, edges


def nearest_node(nodes, x, y):
    return min(range(len(nodes)), key=lambda i: (nodes[i][0] - x) ** 2 + (nodes[i][1] - y) ** 2)


# ---------------------------------------------------------------- gameplay locations
# (id, type, name, district, fx, fy): position as a fraction across the district, snapped to the nearest junction.
POIS = [
    ("safehouse_narrows", "safehouse", "Tolly's Lockup", "narrows", 0.35, 0.45),
    ("safehouse_heights", "safehouse", "Corliss Street Flat", "heights", 0.30, 0.70),
    ("police_lantern", "police", "Lantern Quarter Precinct", "lantern", 0.55, 0.50),
    ("police_heights", "police", "Central Precinct", "heights", 0.60, 0.35),
    ("police_northgate", "police", "Northgate Precinct", "northgate", 0.50, 0.50),
    ("hospital_saltgrass", "hospital", "St. Brannoc Infirmary", "saltgrass", 0.60, 0.50),
    ("hospital_halfmoon", "hospital", "Halfmoon General", "halfmoon", 0.45, 0.40),
    ("shop_guns", "shop", "Kestrel Arms", "ironworks", 0.70, 0.60),
    ("shop_clothes", "shop", "Gull & Thread", "lantern", 0.30, 0.30),
    ("garage_breakwater", "garage", "Pier Nine Motors", "breakwater", 0.25, 0.50),
    ("garage_northgate", "garage", "Hollin's Body Shop", "northgate", 0.25, 0.60),
    ("drydock", "mission", "Number Four Dry Dock", "ironworks", 0.25, 0.35),
    ("customs_yard", "mission", "Customs Yard", "ironworks", 0.50, 0.25),
    ("ledger_a", "mission", "Brass Ring Bookmaker", "lantern", 0.70, 0.75),
    ("ledger_b", "mission", "Pawnbroker's Back Room", "lantern", 0.45, 0.20),
    ("ledger_c", "mission", "Tannery Row Office", "narrows", 0.70, 0.80),
    ("heights_garage", "mission", "Aldergate Car Park", "heights", 0.75, 0.60),
    ("brass_warehouse", "mission", "Brass Ring Warehouse", "breakwater", 0.70, 0.35),
    ("hill_villa", "mission", "Finch-Mercer Residence", "cinder", 0.55, 0.55),
    ("boardwalk", "mission", "Breakwater Boardwalk", "breakwater", 0.50, 0.75),
    ("prop_bar", "property", "The Slack Tide (bar)", "narrows", 0.60, 0.25),
    ("prop_carwash", "property", "Saltgrass Car Wash", "saltgrass", 0.30, 0.50),
    ("prop_boatshed", "property", "Halfmoon Boat Shed", "halfmoon", 0.15, 0.20),
    ("prop_printshop", "property", "Lantern Print Works", "lantern", 0.80, 0.40),
    ("prop_depot", "property", "Northgate Freight Depot", "northgate", 0.75, 0.40),
]


def place_pois(nodes):
    by_id = {d[0]: d for d in DISTRICTS}
    out = []
    for pid, ptype, name, dist, fx, fy in POIS:
        _, _, _, x0, y0, x1, y1, _, _ = by_id[dist]
        n = nearest_node(nodes, x0 + (x1 - x0) * fx, y0 + (y1 - y0) * fy)
        # Just off the junction, so markers and spawn points are not in the middle of the road.
        out.append({"id": pid, "type": ptype, "name": name, "pos": [snap(nodes[n][0] + 9), snap(nodes[n][1] + 9)]})
    return out


def build_missions(pois):
    p = {x["id"]: x["pos"] for x in pois}
    return {"missions": [
        {"id": "m01_slack_water", "title": "Slack Water", "giver": "Tolly Reyne", "start": p["safehouse_narrows"],
         "intro": "Tolly has a van sitting at the dry dock that needs to be somewhere else before the morning shift.",
         "reward": {"cash": 800},
         "objectives": [
             {"type": "goto", "pos": p["drydock"], "radius": 14, "text": "Go to Number Four Dry Dock"},
             {"type": "enter_vehicle", "tag": "van", "text": "Get in the van"},
             {"type": "deliver", "tag": "van", "pos": p["safehouse_narrows"], "radius": 14, "maxHeat": 2,
              "text": "Drive the van to Tolly's lockup without drawing attention"}],
         "fail": [{"type": "vehicle_destroyed", "tag": "van"}, {"type": "player_down"}]},
        {"id": "m02_paper_trail", "title": "Paper Trail", "giver": "Tolly Reyne", "start": p["safehouse_narrows"],
         "requires": ["m01_slack_water"],
         "intro": "The Brass Ring keeps its books in three places. Tolly wants all three.",
         "reward": {"cash": 1400},
         "objectives": [
             {"type": "collect", "tag": "ledger", "count": 3, "text": "Take the three ledgers"},
             {"type": "lose_heat", "text": "Lose the police"},
             {"type": "goto", "pos": p["safehouse_narrows"], "radius": 12, "text": "Bring the ledgers to Tolly"}]},
        {"id": "m03_night_ferry", "title": "Night Ferry", "giver": "Odile Marsh", "start": p["garage_breakwater"],
         "requires": ["m02_paper_trail"],
         "intro": "Odile has a buyer for a coupe parked in the Aldergate car park. The owner has not been asked.",
         "reward": {"cash": 2200},
         "objectives": [
             {"type": "goto", "pos": p["heights_garage"], "radius": 16, "text": "Go to the Aldergate Car Park"},
             {"type": "enter_vehicle", "tag": "coupe", "text": "Take the coupe"},
             {"type": "lose_heat", "text": "Lose the police"},
             {"type": "deliver", "tag": "coupe", "pos": p["garage_breakwater"], "radius": 14, "timeLimit": 300,
              "text": "Deliver the coupe to Pier Nine Motors"}],
         "fail": [{"type": "vehicle_destroyed", "tag": "coupe"}, {"type": "player_down"}]},
        {"id": "m04_brass_tacks", "title": "Brass Tacks", "giver": "Tolly Reyne", "start": p["prop_bar"],
         "requires": ["m02_paper_trail"],
         "intro": "The ledgers named a warehouse on the boardwalk side. The Brass Ring will not hand it over politely.",
         "reward": {"cash": 3000},
         "objectives": [
             {"type": "goto", "pos": p["brass_warehouse"], "radius": 20, "text": "Go to the Brass Ring warehouse"},
             {"type": "eliminate", "tag": "brass_enforcer", "count": 5, "text": "Clear out the enforcers"},
             {"type": "collect", "tag": "manifest", "count": 1, "text": "Find the shipping manifest"},
             {"type": "lose_heat", "text": "Lose the police"}]},
        {"id": "m05_customs", "title": "Customs", "giver": "Odile Marsh", "start": p["garage_breakwater"],
         "requires": ["m03_night_ferry", "m04_brass_tacks"],
         "intro": "The manifest lists a container that clears the Customs Yard tonight. It needs to clear it early.",
         "reward": {"cash": 5000},
         "objectives": [
             {"type": "goto", "pos": p["customs_yard"], "radius": 18, "text": "Get into the Customs Yard"},
             {"type": "wait", "seconds": 45, "text": "Hold the yard while Odile cuts the seals"},
             {"type": "enter_vehicle", "tag": "truck", "text": "Take the truck"},
             {"type": "deliver", "tag": "truck", "pos": p["prop_depot"], "radius": 18, "timeLimit": 420,
              "text": "Drive the truck to the Northgate Freight Depot"}],
         "fail": [{"type": "vehicle_destroyed", "tag": "truck"}, {"type": "player_down"}]},
        {"id": "m06_high_water", "title": "High Water", "giver": "Ines Varga", "start": p["safehouse_heights"],
         "requires": ["m05_customs"],
         "intro": "The container held the Harbor Commissioner's own paperwork. Time to return it in person.",
         "reward": {"cash": 9000},
         "objectives": [
             {"type": "goto", "pos": p["hill_villa"], "radius": 20, "text": "Drive up to the Finch-Mercer residence"},
             {"type": "eliminate", "tag": "villa_guard", "count": 6, "text": "Get past the security detail"},
             {"type": "collect", "tag": "inquiry_file", "count": 1, "text": "Take the inquiry file from the study"},
             {"type": "lose_heat", "text": "Lose the police"},
             {"type": "goto", "pos": p["boardwalk"], "radius": 14, "text": "Meet Tolly on the boardwalk"}]},
    ]}


def build_economy(pois):
    p = {x["id"]: x for x in pois}

    def prop(pid, price, income):
        return {"id": pid, "name": p[pid]["name"], "pos": p[pid]["pos"], "price": price, "incomePerDay": income}

    return {
        "startingCash": 500,
        "items": [
            {"id": "pistol_m9k", "name": "Harlan M9K pistol", "category": "weapon", "price": 650},
            {"id": "smg_wasp", "name": "Wasp-45 submachine gun", "category": "weapon", "price": 2400},
            {"id": "shotgun_dockhand", "name": "Dockhand 12 shotgun", "category": "weapon", "price": 1900},
            {"id": "rifle_crane", "name": "Crane AR-7 rifle", "category": "weapon", "price": 4800},
            {"id": "ammo_pistol", "name": "Pistol rounds (48)", "category": "ammo", "price": 60},
            {"id": "ammo_smg", "name": "SMG rounds (120)", "category": "ammo", "price": 140},
            {"id": "ammo_shotgun", "name": "Shells (24)", "category": "ammo", "price": 110},
            {"id": "ammo_rifle", "name": "Rifle rounds (90)", "category": "ammo", "price": 220},
            {"id": "armor_vest", "name": "Ballistic vest", "category": "consumable", "price": 900},
            {"id": "medkit", "name": "First aid kit", "category": "consumable", "price": 150},
            {"id": "respray", "name": "Respray (clears vehicle description)", "category": "vehicle_mod", "price": 400},
            {"id": "repair", "name": "Full repair", "category": "vehicle_mod", "price": 600},
            {"id": "tune_engine", "name": "Engine tune", "category": "vehicle_mod", "price": 3500},
            {"id": "tyres_wet", "name": "Wet-weather tyres", "category": "vehicle_mod", "price": 1200},
        ],
        "properties": [
            prop("prop_carwash", 18000, 900),
            prop("prop_bar", 32000, 1700),
            prop("prop_boatshed", 45000, 2400),
            prop("prop_printshop", 70000, 3900),
            prop("prop_depot", 120000, 7000),
        ],
    }


# ---------------------------------------------------------------- map preview
def write_svg(path, city):
    x0, y0, x1, y1 = BOUNDS
    scale = 0.2

    def X(x):
        return round((x - x0) * scale, 1)

    def Y(y):
        return round((y1 - y) * scale, 1)   # flip: north is up

    fills = {"industrial": "#c9c2b6", "canal": "#d6c7ba", "oldtown": "#e3cfa9", "residential": "#d5dcc2", "suburb": "#cfe0c4",
             "beach": "#f0e2b8", "downtown": "#c8cfdc", "hillside": "#bfd3b4"}
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %g %g" font-family="Helvetica, Arial, sans-serif">' % (X(x1), Y(y0)),
           '<rect width="100%" height="100%" fill="#e9eef0"/>',
           '<rect x="0" y="%g" width="%g" height="%g" fill="#9fc3d6"/>' % (Y(BAY_Y), X(x1), Y(y0) - Y(BAY_Y)),
           '<rect x="%g" y="0" width="%g" height="%g" fill="#9fc3d6"/>' % (X(RIVER[0]), X(RIVER[1]) - X(RIVER[0]), Y(y0))]
    for d in city["districts"]:
        (ax, ay), _, (bx, by), _ = d["poly"]
        out.append('<rect x="%g" y="%g" width="%g" height="%g" fill="%s"/>' % (X(ax), Y(by), X(bx) - X(ax), Y(ay) - Y(by), fills[d["kind"]]))
    for cx, cy, w, dp, h, _ in city["buildings"]:
        shade = max(90, 200 - int(h * 0.6))
        out.append('<rect x="%g" y="%g" width="%g" height="%g" fill="rgb(%d,%d,%d)"/>' % (X(cx - w / 2), Y(cy + dp / 2), round(w * scale, 1), round(dp * scale, 1), shade, shade, shade + 8))
    nodes = city["roads"]["nodes"]
    styles = {0: ("#ffffff", 1.0), 1: ("#ffffff", 1.4), 2: ("#f7d98a", 2.6), 3: ("#f0a35e", 3.4), 4: ("#8a8f98", 3.0)}
    for kind in (1, 2, 4, 3):
        color, width = styles[kind]
        segs = ["M%g %gL%g %g" % (X(nodes[a][0]), Y(nodes[a][1]), X(nodes[b][0]), Y(nodes[b][1])) for a, b, _, _, k, _ in city["roads"]["edges"] if k == kind]
        out.append('<path d="%s" stroke="%s" stroke-width="%g" fill="none" stroke-linecap="round"/>' % ("".join(segs), color, width))
    for d in city["districts"]:
        (ax, ay), _, (bx, by), _ = d["poly"]
        out.append('<text x="%g" y="%g" font-size="26" font-weight="700" fill="#1f2a33" fill-opacity="0.75" text-anchor="middle">%s</text>' % (X((ax + bx) / 2), Y((ay + by) / 2), d["name"].upper()))
    colors = {"safehouse": "#1c7c54", "police": "#2456a6", "hospital": "#c0392b", "shop": "#8e44ad", "garage": "#8e44ad", "mission": "#d35400", "property": "#b7950b"}
    for poi in city["pois"]:
        out.append('<circle cx="%g" cy="%g" r="7" fill="%s" stroke="#fff" stroke-width="2"><title>%s</title></circle>' % (X(poi["pos"][0]), Y(poi["pos"][1]), colors[poi["type"]], html.escape(poi["name"])))
    out.append('<text x="%g" y="%g" font-size="22" fill="#2f5d73" text-anchor="middle" transform="rotate(-90 %g %g)">WREN RIVER</text>' % (X(3100), Y(2700), X(3100), Y(2700)))
    out.append('<text x="%g" y="%g" font-size="24" fill="#2f5d73" text-anchor="middle">MERIDIAN BAY</text>' % (X(3200), Y(250)))
    lx, ly = 24, 24
    out.append('<rect x="%d" y="%d" width="250" height="%d" fill="#ffffff" fill-opacity="0.9" rx="6"/>' % (lx - 10, ly - 14, 22 * len(colors) + 34))
    out.append('<text x="%d" y="%d" font-size="16" font-weight="700" fill="#1f2a33">Port Meridian (blockout)</text>' % (lx, ly + 6))
    for i, (kind, color) in enumerate(colors.items()):
        out.append('<circle cx="%d" cy="%d" r="6" fill="%s"/><text x="%d" y="%d" font-size="14" fill="#1f2a33">%s</text>' % (lx + 6, ly + 28 + i * 22, color, lx + 20, ly + 33 + i * 22, kind))
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out))


# ---------------------------------------------------------------- main
def build_city(seed=SEED):
    b = Builder(seed)
    for idx, d in enumerate(DISTRICTS):
        b.district(idx, d)
    b.stitch()
    nodes, edges = b.finish()
    districts = [{"id": d[0], "name": d[1], "kind": d[2], "density": d[7], "wealth": d[8],
                  "poly": [[d[3], d[4]], [d[5], d[4]], [d[5], d[6]], [d[3], d[6]]]} for d in DISTRICTS]
    return {
        "meta": {"name": "Port Meridian", "seed": seed, "units": "meters", "bounds": list(BOUNDS)},
        "districts": districts,
        "roads": {"kinds": ROAD_KINDS, "nodes": nodes, "edges": edges},
        "buildings": b.buildings,
        "pois": place_pois(nodes),
        "water": [{"name": "Wren River", "poly": [[RIVER[0], BAY_Y], [RIVER[1], BAY_Y], [RIVER[1], BOUNDS[3]], [RIVER[0], BOUNDS[3]]]},
                  {"name": "Meridian Bay", "poly": [[BOUNDS[0], BOUNDS[1]], [BOUNDS[2], BOUNDS[1]], [BOUNDS[2], BAY_Y], [BOUNDS[0], BAY_Y]]}],
        "bridges": [{"name": name, "y": y} for name, y in BRIDGES],
    }


def dump(path, obj, compact_lists=True):
    """JSON with one array-of-numbers per line: small enough to commit, still readable in a diff."""
    def enc(o, level):
        pad = " " * level
        if isinstance(o, dict):
            if not o:
                return "{}"
            return "{\n" + ",\n".join('%s %s: %s' % (pad, json.dumps(k), enc(v, level + 1)) for k, v in o.items()) + "\n" + pad + "}"
        if isinstance(o, list):
            if not o:
                return "[]"
            if all(not isinstance(v, (dict, list)) for v in o):
                return json.dumps(o, separators=(",", ":"))
            return "[\n" + ",\n".join(pad + " " + enc(v, level + 1) for v in o) + "\n" + pad + "]"
        return json.dumps(o)
    with open(path, "w") as f:
        f.write(enc(obj, 0) + "\n")


def main():
    data_dir = os.path.join(ROOT, "Content", "Data")
    os.makedirs(data_dir, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "Docs"), exist_ok=True)
    city = build_city()
    dump(os.path.join(data_dir, "city.json"), city)
    dump(os.path.join(data_dir, "missions.json"), build_missions(city["pois"]))
    dump(os.path.join(data_dir, "economy.json"), build_economy(city["pois"]))
    write_svg(os.path.join(ROOT, "Docs", "map.svg"), city)
    roads = city["roads"]
    km = sum(math.dist(roads["nodes"][a][:2], roads["nodes"][b][:2]) for a, b, *_ in roads["edges"]) / 1000
    print("Port Meridian: %d districts, %d junctions, %d roads (%.0f km), %d buildings, %d locations" % (
        len(city["districts"]), len(roads["nodes"]), len(roads["edges"]), km, len(city["buildings"]), len(city["pois"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
