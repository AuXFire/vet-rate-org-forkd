#!/usr/bin/env python3
"""Turn OpenStreetMap data into the road tiles the game streams.

    python3 Tools/osm_to_tiles.py region.osm --out Content/Data/World/tiles
    python3 Tools/osm_to_tiles.py london.osm.bz2 --out Content/Data/World/tiles --drive left --preview london.svg

Input: OpenStreetMap XML (.osm, optionally .gz or .bz2) or Overpass JSON. Several files may be given; they are
merged. For .osm.pbf extracts (geofabrik.de), convert first:  osmium cat region.osm.pbf -o region.osm.bz2

Output: one JSON file per web-map tile at <out>/<zoom>/<x>/<y>.json, in the format documented in
Source/Undertow/Sim/UtWorld.h, plus <out>/index.json describing the run.

What it keeps: roads a car can drive on. Junctions are nodes shared by more than one road. Bends are kept to
within --tolerance meters. Long stretches are cut so that no edge is longer than --max-edge. An edge is
written into the tile of each of its ends, so a road crossing a tile border appears in both tiles with the
same node ids.

Standard library only. It holds the whole input in memory, so run it per city or region, not on the planet.
OpenStreetMap data is (c) OpenStreetMap contributors, licensed under the ODbL: credit them in the game.
"""
import argparse
import bz2
import gzip
import hashlib
import json
import math
import os
import sys
import xml.etree.ElementTree as ET

# highway value -> (kind index, lanes per direction, speed km/h) used when the way has no tags of its own.
# Kind indices match ut::RoadKind: 0 alley, 1 street, 2 arterial, 3 highway, 4 bridge.
ROAD_CLASSES = {
    "motorway": (3, 2, 110), "motorway_link": (3, 1, 60),
    "trunk": (3, 2, 90), "trunk_link": (3, 1, 50),
    "primary": (2, 1, 60), "primary_link": (2, 1, 40),
    "secondary": (2, 1, 50), "secondary_link": (2, 1, 40),
    "tertiary": (1, 1, 50), "tertiary_link": (1, 1, 40),
    "unclassified": (1, 1, 50), "residential": (1, 1, 40),
    "living_street": (0, 1, 10), "service": (0, 1, 20),
}
SKIPPED_SERVICE = {"parking_aisle", "driveway", "drive-through", "emergency_access"}
ZONE_SPEEDS = {"urban": 50, "rural": 90, "motorway": 120, "living_street": 10, "trunk": 100, "zone30": 30, "zone20": 20}
SYNTHETIC_ID_BASE = 1 << 50          # ids for nodes this tool adds; OpenStreetMap's own ids are far below this
EARTH_RADIUS = 6371008.8


def open_text(path):
    if path.endswith(".gz"):
        return gzip.open(path, "rb")
    if path.endswith(".bz2"):
        return bz2.open(path, "rb")
    return open(path, "rb")


def read_osm(path, ways, nodes):
    """Adds every tagged way and every node from one file to `ways` {id: (refs, tags)} and `nodes` {id: (lat, lon, signal)}."""
    with open_text(path) as f:
        head = f.read(64).lstrip()
    if head.startswith(b"{"):
        with open_text(path) as f:
            for el in json.load(f).get("elements", []):
                if el.get("type") == "node":
                    signal = el.get("tags", {}).get("highway") == "traffic_signals"
                    nodes[el["id"]] = (el["lat"], el["lon"], signal)
                elif el.get("type") == "way" and "highway" in el.get("tags", {}):
                    ways[el["id"]] = (el.get("nodes", []), el["tags"])
        return
    if head.startswith(b"\x0a\x09") or path.endswith(".pbf"):
        sys.exit(f"{path}: .pbf is not supported directly. Convert it first: osmium cat {path} -o region.osm.bz2")
    with open_text(path) as f:
        for _, el in ET.iterparse(f, events=("end",)):
            if el.tag == "node":
                signal = any(t.get("k") == "highway" and t.get("v") == "traffic_signals" for t in el.iter("tag"))
                nodes[int(el.get("id"))] = (float(el.get("lat")), float(el.get("lon")), signal)
                el.clear()
            elif el.tag == "way":
                tags = {t.get("k"): t.get("v") for t in el.iter("tag")}
                if "highway" in tags:
                    ways[int(el.get("id"))] = ([int(n.get("ref")) for n in el.iter("nd")], tags)
                el.clear()
            elif el.tag == "relation":
                el.clear()


def drivable(tags, all_service=False):
    if tags.get("highway") not in ROAD_CLASSES or tags.get("area") == "yes":
        return False
    if tags.get("access") in ("no", "private") or tags.get("motor_vehicle") == "no" or tags.get("vehicle") == "no":
        return False
    if tags.get("motorcar") == "no":
        return False
    if tags["highway"] == "service" and not all_service and tags.get("service") in SKIPPED_SERVICE:
        return False
    return True


def first_int(text):
    digits = ""
    for ch in text or "":
        if ch.isdigit():
            digits += ch
        elif digits:
            break
    return int(digits) if digits else None


def speed_mps(tags, default_kmh):
    raw = (tags.get("maxspeed") or "").strip().lower()
    kmh = default_kmh
    if raw:
        number = first_int(raw)
        if raw in ("none", "no"):
            kmh = 130
        elif raw == "walk":
            kmh = 7
        elif number is not None and raw[0].isdigit():
            kmh = number * (1.609344 if "mph" in raw else 1.0)
        elif ":" in raw:                                   # "DE:urban", "FR:rural", "GB:nsl_single", ...
            kmh = ZONE_SPEEDS.get(raw.split(":", 1)[1], default_kmh)
    return round(min(max(kmh, 5), 160) / 3.6, 1)


def one_way(tags):
    """Returns 0 (two-way), 1 (one-way in drawing order) or -1 (one-way against it)."""
    value = tags.get("oneway")
    if value in ("-1", "reverse"):
        return -1
    if value in ("yes", "true", "1"):
        return 1
    if value in ("no", "false", "0"):
        return 0
    if tags.get("junction") in ("roundabout", "circular") or tags.get("highway") == "motorway":
        return 1
    return 0


def lanes_per_direction(tags, oneway, default):
    total = first_int(tags.get("lanes"))
    if total is None:
        return default
    if oneway:
        return min(max(total, 1), 6)
    forward = first_int(tags.get("lanes:forward"))
    return min(max(forward if forward else total // 2, 1), 6)


def local_xy(lat, lon, lat0, lon0):
    return (math.radians(lon - lon0) * EARTH_RADIUS * math.cos(math.radians(lat0)), math.radians(lat - lat0) * EARTH_RADIUS)


def simplify(points, tolerance):
    """Douglas-Peucker on [(x, y), ...]: returns the indices to keep."""
    keep = [0, len(points) - 1]
    stack = [(0, len(points) - 1)]
    while stack:
        a, b = stack.pop()
        if b - a < 2:
            continue
        ax, ay = points[a]
        bx, by = points[b]
        dx, dy = bx - ax, by - ay
        length_sq = dx * dx + dy * dy
        worst, worst_d = -1, tolerance
        for i in range(a + 1, b):
            px, py = points[i]
            if length_sq > 0:
                t = min(max(((px - ax) * dx + (py - ay) * dy) / length_sq, 0.0), 1.0)
                d = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
            else:
                d = math.hypot(px - ax, py - ay)
            if d > worst_d:
                worst, worst_d = i, d
        if worst >= 0:
            keep.append(worst)
            stack.append((a, worst))
            stack.append((worst, b))
    return sorted(keep)


def synthetic_id(a, b, k):
    digest = hashlib.blake2b(f"{a}:{b}:{k}".encode(), digest_size=6).digest()
    return SYNTHETIC_ID_BASE + int.from_bytes(digest, "big")


def tile_of(lat, lon, zoom):
    n = 1 << zoom
    lat = min(max(lat, -85.0511287798066), 85.0511287798066)
    x = int(math.floor((lon + 180.0) / 360.0 * n))
    y = int(math.floor((1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n))
    return min(max(x, 0), n - 1), min(max(y, 0), n - 1)


def tile_centre(x, y, zoom):
    n = 1 << zoom
    lon = (x + 0.5) / n * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * (y + 0.5) / n))))
    return lat, lon


def build_graph(ways, nodes, tolerance=1.5, max_edge=250.0, all_service=False):
    """Returns (graph_nodes {id: (lat, lon, signal)}, edges [(from, to, lanes, speed, kind, oneway)])."""
    roads = []
    uses = {}
    for way_id in sorted(ways):
        refs, tags = ways[way_id]
        refs = [r for r in refs if r in nodes]
        if len(refs) < 2 or not drivable(tags, all_service):
            continue
        roads.append((refs, tags))
        for r in refs:
            uses[r] = uses.get(r, 0) + 1
        uses[refs[0]] += 1                                  # ends always count as junctions
        uses[refs[-1]] += 1

    out_nodes = {}
    edges = []
    seen = set()
    for refs, tags in roads:
        kind, default_lanes, default_kmh = ROAD_CLASSES[tags["highway"]]
        if tags.get("bridge") not in (None, "no"):
            kind = 4
        direction = one_way(tags)
        if direction < 0:
            refs = refs[::-1]
        oneway = 1 if direction else 0
        lanes = lanes_per_direction(tags, oneway, default_lanes)
        speed = speed_mps(tags, default_kmh)

        # Cut the way at junctions and signals; simplify the bends in between.
        cuts = [i for i, r in enumerate(refs) if uses[r] >= 2 or nodes[r][2]]
        chain = []
        for a, b in zip(cuts, cuts[1:]):
            lat0, lon0 = nodes[refs[a]][0], nodes[refs[a]][1]
            pts = [local_xy(nodes[r][0], nodes[r][1], lat0, lon0) for r in refs[a:b + 1]]
            kept = [a + i for i in simplify(pts, tolerance)]
            chain.extend(kept if not chain else kept[1:])
        for i, j in zip(chain, chain[1:]):
            a, b = refs[i], refs[j]
            if a == b:
                continue
            key = (a, b) if oneway else (min(a, b), max(a, b))
            if key in seen:
                continue
            seen.add(key)
            (alat, alon, asig), (blat, blon, bsig) = nodes[a], nodes[b]
            length = math.hypot(*local_xy(blat, blon, alat, alon))
            pieces = max(1, int(math.ceil(length / max_edge)))
            previous = a
            out_nodes[a] = (alat, alon, asig)
            out_nodes[b] = (blat, blon, bsig)
            for k in range(1, pieces):                      # split a long straight into equal parts
                t = k / pieces
                mid = synthetic_id(a, b, k)
                out_nodes[mid] = (alat + (blat - alat) * t, alon + (blon - alon) * t, False)
                edges.append((previous, mid, lanes, speed, kind, oneway))
                previous = mid
            edges.append((previous, b, lanes, speed, kind, oneway))
    return out_nodes, edges


def make_tiles(graph_nodes, edges, zoom):
    """Returns {(x, y): (node id set, edge list)}: every edge goes into the tile of each of its ends."""
    tiles = {}
    for edge in edges:
        for end in (edge[0], edge[1]):
            key = tile_of(graph_nodes[end][0], graph_nodes[end][1], zoom)
            ids, tile_edges = tiles.setdefault(key, (set(), []))
            if not tile_edges or tile_edges[-1] is not edge:
                tile_edges.append(edge)
            ids.update((edge[0], edge[1]))
    return tiles


def tile_json(x, y, zoom, ids, tile_edges, graph_nodes, left):
    doc = {
        "z": zoom, "x": x, "y": y, "left": 1 if left else 0,
        "nodes": [[i, round(graph_nodes[i][0], 7), round(graph_nodes[i][1], 7), 1 if graph_nodes[i][2] else 0] for i in sorted(ids)],
        "edges": [list(e) for e in sorted(tile_edges)],
    }
    return json.dumps(doc, separators=(",", ":"))


def write_preview(path, graph_nodes, edges, tiles, zoom):
    lats = [n[0] for n in graph_nodes.values()]
    lons = [n[1] for n in graph_nodes.values()]
    lat0, lon0 = min(lats), min(lons)
    width, height = local_xy(max(lats), max(lons), lat0, lon0)
    scale = 1400.0 / max(width, height, 1.0)
    colours = ["#8a8f98", "#cfd3da", "#f2c14e", "#f08a4b", "#6fb1e0"]
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width * scale + 20:.0f} {height * scale + 20:.0f}" style="background:#15181d">']

    def at(lat, lon):
        x, y = local_xy(lat, lon, lat0, lon0)
        return 10 + x * scale, 10 + (height - y) * scale

    n = 1 << zoom
    for (tx, ty) in tiles:                                  # tile outlines
        west, east = tx / n * 360 - 180, (tx + 1) / n * 360 - 180
        north = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * ty / n))))
        south = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (ty + 1) / n))))
        (x0, y0), (x1, y1) = at(north, west), at(south, east)
        out.append(f'<rect x="{x0:.1f}" y="{y0:.1f}" width="{x1 - x0:.1f}" height="{y1 - y0:.1f}" fill="none" stroke="#2c3440"/>')
    for a, b, lanes, _speed, kind, _oneway in sorted(edges, key=lambda e: e[4] in (2, 3)):
        (x0, y0), (x1, y1) = at(*graph_nodes[a][:2]), at(*graph_nodes[b][:2])
        out.append(f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" y2="{y1:.1f}" stroke="{colours[kind]}" stroke-width="{0.7 + 0.5 * lanes:.1f}" stroke-linecap="round"/>')
    for lat, lon, signal in graph_nodes.values():
        if signal:
            x, y = at(lat, lon)
            out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="2.2" fill="#e5484d"/>')
    out.append("</svg>")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


def convert(inputs, out_dir, zoom=14, tolerance=1.5, max_edge=250.0, left=False, bbox=None, all_service=False, preview=None):
    ways, nodes = {}, {}
    for path in inputs:
        read_osm(path, ways, nodes)
    graph_nodes, edges = build_graph(ways, nodes, tolerance, max_edge, all_service)
    tiles = make_tiles(graph_nodes, edges, zoom)
    if bbox:                                                # keep tiles whose centre is inside the trusted area
        south, west, north, east = bbox
        tiles = {k: v for k, v in tiles.items()
                 if south <= tile_centre(k[0], k[1], zoom)[0] <= north and west <= tile_centre(k[0], k[1], zoom)[1] <= east}
    for (x, y), (ids, tile_edges) in sorted(tiles.items()):
        folder = os.path.join(out_dir, str(zoom), str(x))
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, f"{y}.json"), "w", encoding="utf-8") as f:
            f.write(tile_json(x, y, zoom, ids, tile_edges, graph_nodes, left))
    lats = [n[0] for n in graph_nodes.values()] or [0.0]
    lons = [n[1] for n in graph_nodes.values()] or [0.0]
    index = {
        "zoom": zoom,
        "drive": "left" if left else "right",
        "tiles": sorted([x, y] for x, y in tiles),
        "junctions": len(graph_nodes),
        "roads": len(edges),
        "bounds": [round(min(lats), 6), round(min(lons), 6), round(max(lats), 6), round(max(lons), 6)],
        "sources": [os.path.basename(p) for p in inputs],
        "attribution": "Road data (c) OpenStreetMap contributors, Open Database License (ODbL) 1.0. https://www.openstreetmap.org/copyright",
    }
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=1)
    if preview:
        write_preview(preview, graph_nodes, edges, tiles, zoom)
    return index


def main(argv=None):
    ap = argparse.ArgumentParser(description="Convert OpenStreetMap data into Undertow road tiles.")
    ap.add_argument("inputs", nargs="+", help=".osm / .osm.gz / .osm.bz2 or Overpass .json files")
    ap.add_argument("--out", required=True, help="output folder, e.g. Content/Data/World/tiles")
    ap.add_argument("--zoom", type=int, default=14, help="tile zoom level; must match WorldRoads::zoom (default 14)")
    ap.add_argument("--tolerance", type=float, default=1.5, help="how far, in meters, a simplified road may stray from the original")
    ap.add_argument("--max-edge", type=float, default=250.0, help="longest edge in meters before a road is cut")
    ap.add_argument("--drive", choices=("right", "left"), default="right", help="which side traffic keeps to in this area")
    ap.add_argument("--bbox", help="south,west,north,east: only write tiles whose centre is inside")
    ap.add_argument("--all-service", action="store_true", help="keep driveways and parking aisles too")
    ap.add_argument("--preview", help="also write an SVG picture of the result")
    args = ap.parse_args(argv)
    bbox = tuple(float(v) for v in args.bbox.split(",")) if args.bbox else None
    if bbox and len(bbox) != 4:
        ap.error("--bbox needs four numbers: south,west,north,east")
    index = convert(args.inputs, args.out, args.zoom, args.tolerance, args.max_edge, args.drive == "left", bbox, args.all_service, args.preview)
    print(f"{len(index['tiles'])} tiles, {index['junctions']} junctions, {index['roads']} road segments -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
