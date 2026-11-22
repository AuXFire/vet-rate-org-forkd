"""The OpenStreetMap converter, on a small hand-written map and on the shipped Monaco sample."""
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "Tools"))
import osm_to_tiles as osm  # noqa: E402

# A crossroads at node 5 with a signal, a one-way street drawn backwards, a roundabout, a long straight, a
# bendy lane, and things a car cannot use.
#
#        2
#        |
#   1 -- 5 -- 3 -------------------------------- 9      (1-5-3: "Main St", 3-9: 1.1 km straight)
#        |
#        4 ~~ 41 ~~ 42 ~~ 43 (lane: 41 is 0.4 m off the straight line, 42 is a real corner)
LAT, LON = 45.0, 7.0
M = 1.0 / 111195.0            # one meter of latitude, in degrees
ME = M / math.cos(math.radians(LAT))


def node(i, east, north, extra=""):
    return f'<node id="{i}" lat="{LAT + north * M:.8f}" lon="{LON + east * ME:.8f}">{extra}</node>'


def way(i, refs, **tags):
    nds = "".join(f'<nd ref="{r}"/>' for r in refs)
    tgs = "".join(f'<tag k="{k.replace("__", ":")}" v="{v}"/>' for k, v in tags.items())
    return f'<way id="{i}">{nds}{tgs}</way>'


SIGNAL = '<tag k="highway" v="traffic_signals"/>'
OSM_XML = "<osm>" + "".join([
    node(1, -100, 0), node(2, 0, 100), node(3, 100, 0), node(4, 0, -100), node(5, 0, 0, SIGNAL),
    node(9, 1200, 0),
    node(41, 50, -100.4), node(42, 100, -100), node(43, 150, -130),
    node(20, 300, 60), node(21, 320, 80), node(22, 340, 60), node(23, 320, 40),   # roundabout
    node(30, -100, 50), node(31, -50, 50),                                        # footpath
    node(50, 500, 0), node(51, 500, 80),                                          # private road off the straight
    way(100, [1, 5, 3], highway="primary", lanes="4", maxspeed="30 mph", name="Main St"),
    way(101, [2, 5, 4], highway="residential"),
    way(102, [9, 50, 3], highway="secondary", oneway="-1", maxspeed="70"),         # drawn east to west, runs west to east
    way(103, [4, 41, 42, 43], highway="service"),
    way(104, [20, 21, 22, 23, 20], highway="tertiary", junction="roundabout"),
    way(105, [30, 31], highway="footway"),
    way(106, [50, 51], highway="residential", access="private"),
    way(107, [1, 30], highway="service", service="driveway"),
    way(108, [2, 30], highway="residential", bridge="yes", maxspeed="FR:urban"),
]) + "</osm>"


def convert(tmp_path, **kw):
    src = tmp_path / "map.osm"
    src.write_text(OSM_XML)
    out = tmp_path / "tiles"
    index = osm.convert([str(src)], str(out), **kw)
    return index, out


def graph():
    ways, nodes = {}, {}
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".osm", delete=False) as f:
        f.write(OSM_XML)
    osm.read_osm(f.name, ways, nodes)
    os.unlink(f.name)
    return osm.build_graph(ways, nodes)


def edge(edges, a, b):
    found = [e for e in edges if (e[0], e[1]) == (a, b) or (not e[5] and (e[0], e[1]) == (b, a))]
    assert len(found) == 1, (a, b, found)
    return found[0]


def test_only_drivable_roads_are_kept():
    nodes, edges = graph()
    ids = set(nodes)
    assert 31 not in ids                       # the footpath's far end
    assert 51 not in ids                       # the private road
    assert not any({e[0], e[1]} == {1, 30} for e in edges)   # the driveway
    assert {e[0] for e in edges} | {e[1] for e in edges} == ids


def test_tags_become_lanes_speeds_and_kinds():
    _, edges = graph()
    main = edge(edges, 1, 5)
    assert main[2] == 2                        # lanes=4 on a two-way road: two each way
    assert abs(main[3] - 13.4) < 0.05          # 30 mph
    assert main[4] == 2 and main[5] == 0       # arterial, two-way
    side = edge(edges, 2, 5)
    assert side[2:] == (1, round(40 / 3.6, 1), 1, 0)
    bridge = edge(edges, 2, 30)
    assert bridge[4] == 4 and abs(bridge[3] - 13.9) < 0.05   # "FR:urban" = 50 km/h


def test_one_way_streets_run_the_right_way():
    nodes, edges = graph()
    # Way 102 was drawn 9 -> 50 -> 3 with oneway=-1, so traffic runs 3 -> 9.
    chain, at = [3], 3
    while at != 9:
        nxt = [e for e in edges if e[0] == at and e[5] == 1]
        assert len(nxt) == 1, (at, nxt)
        at = nxt[0][1]
        chain.append(at)
    assert not any(e[1] == 3 and e[5] == 1 for e in edges)
    # The roundabout is one-way in drawing order, all the way round.
    ring = {(e[0], e[1]) for e in edges if e[0] in (20, 21, 22, 23)}
    assert ring == {(20, 21), (21, 22), (22, 23), (23, 20)}
    assert all(edge(edges, a, b)[5] == 1 for a, b in ring)


def test_long_straights_are_cut_and_bends_are_kept():
    nodes, edges = graph()
    # 3 -> 9 is 1.1 km. Node 50 stays in the data but is no longer a junction (its side road was private), so
    # the whole straight is one run, cut into equal parts of at most 250 m.
    assert 50 not in nodes
    lengths = []
    at = 3
    while at != 9:
        e = [e for e in edges if e[0] == at and e[5] == 1][0]
        a, b = nodes[e[0]], nodes[e[1]]
        lengths.append(math.hypot(*osm.local_xy(b[0], b[1], a[0], a[1])))
        at = e[1]
    assert len(lengths) == 5
    assert max(lengths) <= 250.0 and min(lengths) > 200.0
    assert abs(sum(lengths) - 1100.0) < 2.0
    assert all(i >= osm.SYNTHETIC_ID_BASE for i in nodes if i not in range(1, 100))
    # Node 41 is 0.4 m off the straight line 4 -> 42: simplified away. The real corner at 42 stays.
    assert 41 not in nodes and 42 in nodes and 43 in nodes
    assert edge(edges, 4, 42) and edge(edges, 42, 43)


def test_signals_and_junctions():
    nodes, _ = graph()
    assert nodes[5][2] is True
    assert sum(1 for n in nodes.values() if n[2]) == 1


def test_tiles_share_border_nodes_and_are_deterministic(tmp_path):
    # Zoom 18 tiles are about 100 m across here, so this small map spans several.
    index, out = convert(tmp_path, zoom=18)
    assert len(index["tiles"]) >= 6
    assert index["attribution"].startswith("Road data (c) OpenStreetMap contributors")
    tiles = {}
    for x, y in index["tiles"]:
        with open(out / "18" / str(x) / f"{y}.json") as f:
            tiles[(x, y)] = json.load(f)
    all_edges, by_node = set(), {}
    for (x, y), tile in tiles.items():
        assert (tile["z"], tile["x"], tile["y"], tile["left"]) == (18, x, y, 0)
        ids = {n[0] for n in tile["nodes"]}
        assert len(ids) == len(tile["nodes"])
        for n in tile["nodes"]:
            assert by_node.setdefault(n[0], n[1:]) == n[1:]        # the same node is identical in every tile
        for e in tile["edges"]:
            assert e[0] in ids and e[1] in ids                      # every tile stands on its own
            all_edges.add(tuple(e))
            # ...and holds the edge because one of its ends is inside it.
            assert (x, y) in {osm.tile_of(by_node[e[0]][0], by_node[e[0]][1], 18), osm.tile_of(by_node[e[1]][0], by_node[e[1]][1], 18)}
    _, edges = graph()
    assert all_edges == {tuple(e) for e in edges}                   # nothing lost, nothing invented
    crossing = [e for e in edges if osm.tile_of(*by_node[e[0]][:2], 18) != osm.tile_of(*by_node[e[1]][:2], 18)]
    assert crossing
    for e in crossing:                                              # border roads are in both tiles
        assert sum(1 for t in tiles.values() if list(e) in t["edges"]) == 2
    # Same input, same bytes.
    first = {k: json.dumps(v, sort_keys=True) for k, v in tiles.items()}
    index2, out2 = convert(tmp_path, zoom=18)
    for x, y in index2["tiles"]:
        with open(out2 / "18" / str(x) / f"{y}.json") as f:
            assert json.dumps(json.load(f), sort_keys=True) == first[(x, y)]


def test_left_hand_traffic_and_bbox(tmp_path):
    index, out = convert(tmp_path, zoom=18, left=True, bbox=(LAT - 20 * M, LON - 150 * ME, LAT + 150 * M, LON + 150 * ME))
    assert index["drive"] == "left"
    assert 1 <= len(index["tiles"]) <= 9
    x, y = index["tiles"][0]
    with open(out / "18" / str(x) / f"{y}.json") as f:
        assert json.load(f)["left"] == 1


def test_overpass_json_input_matches_xml(tmp_path):
    ways, nodes = {}, {}
    src = tmp_path / "map.osm"
    src.write_text(OSM_XML)
    osm.read_osm(str(src), ways, nodes)
    elements = [{"type": "node", "id": i, "lat": n[0], "lon": n[1], **({"tags": {"highway": "traffic_signals"}} if n[2] else {})} for i, n in nodes.items()]
    elements += [{"type": "way", "id": i, "nodes": refs, "tags": tags} for i, (refs, tags) in ways.items()]
    as_json = tmp_path / "map.json"
    as_json.write_text(json.dumps({"elements": elements}))
    ways2, nodes2 = {}, {}
    osm.read_osm(str(as_json), ways2, nodes2)
    assert osm.build_graph(ways2, nodes2) == osm.build_graph(ways, nodes)


def test_speed_and_lane_parsing_edge_cases():
    assert osm.speed_mps({"maxspeed": "50"}, 30) == round(50 / 3.6, 1)
    assert osm.speed_mps({"maxspeed": "20 mph"}, 30) == round(20 * 1.609344 / 3.6, 1)
    assert osm.speed_mps({"maxspeed": "none"}, 30) == round(130 / 3.6, 1)
    assert osm.speed_mps({"maxspeed": "signals"}, 30) == round(30 / 3.6, 1)
    assert osm.speed_mps({"maxspeed": "DE:rural"}, 30) == round(90 / 3.6, 1)
    assert osm.speed_mps({}, 30) == round(30 / 3.6, 1)
    assert osm.lanes_per_direction({"lanes": "3"}, 1, 1) == 3
    assert osm.lanes_per_direction({"lanes": "3"}, 0, 1) == 1
    assert osm.lanes_per_direction({"lanes": "5", "lanes:forward": "3"}, 0, 1) == 3
    assert osm.lanes_per_direction({"lanes": "2;3"}, 1, 1) == 2
    assert osm.lanes_per_direction({"lanes": "many"}, 0, 2) == 2
    assert osm.one_way({"highway": "motorway"}) == 1
    assert osm.one_way({"highway": "motorway", "oneway": "no"}) == 0
    assert osm.tile_of(51.5074, -0.1278, 14) == (8186, 5448)


def test_shipped_monaco_sample_is_sound():
    base = os.path.join(ROOT, "Content", "Data", "World", "tiles")
    with open(os.path.join(base, "index.json")) as f:
        index = json.load(f)
    assert index["zoom"] == 14 and len(index["tiles"]) >= 6
    assert "OpenStreetMap" in index["attribution"]
    assert os.path.exists(os.path.join(ROOT, "Content", "Data", "World", "ATTRIBUTION.md"))
    total_nodes = {}
    for x, y in index["tiles"]:
        with open(os.path.join(base, "14", str(x), f"{y}.json")) as f:
            tile = json.load(f)
        ids = {n[0] for n in tile["nodes"]}
        for n in tile["nodes"]:
            assert 43.6 < n[1] < 43.9 and 7.3 < n[2] < 7.6          # it really is Monaco
            total_nodes[n[0]] = n
        for e in tile["edges"]:
            assert e[0] in ids and e[1] in ids and e[0] != e[1]
            assert 1 <= e[2] <= 6 and 1.0 <= e[3] <= 45.0 and 0 <= e[4] <= 4 and e[5] in (0, 1)
            a, b = total_nodes[e[0]], total_nodes[e[1]]
            assert math.hypot(*osm.local_xy(b[1], b[2], a[1], a[2])) <= 252.0
    assert len(total_nodes) == index["junctions"]
