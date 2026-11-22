# Undertow: design

## The game in one paragraph

Ines Varga was a harbor pilot in Port Meridian, one of the people who climb aboard container ships and steer them through the channel. An inquiry she is sure was rigged took her licence. With no way to work the water legally, she starts driving for Tolly Reyne, who moves things through the port that the Harbor Authority does not see. The player takes Ines from running a van across town to pulling apart the arrangement between the Harbor Commissioner and the Brass Ring syndicate that cost her the job.

It is a third-person open-world crime game: drive, shoot, steal, run from the police, take missions, buy property.

## Pillars

1. **The city reacts.** Crimes matter only if someone sees them. Witnesses phone it in, traffic panics at gunfire, police search where you were last seen, not where you are.
2. **Weather is a mechanic.** Port Meridian is wet. Rain cuts grip, fog cuts how far police can see, storms empty the pavements. A getaway in fog plays differently from one at noon.
3. **Escapes are earned.** Losing the police means breaking line of sight and getting out of the search area, not waiting out a timer in the open.
4. **Everything is data.** Missions, prices, vehicles, weapons and tuning live in JSON, so content does not need a programmer.

## The world

The world is the Earth at full scale. Terrain, imagery and buildings are streamed; roads come from OpenStreetMap; traffic, police, missions and the economy run on those roads anywhere on the globe. A player can drive out of one city and, hours later, into another.

Streamed scenery is good from a distance and rough up close, so the places where the story happens are built by hand and pinned to a latitude and longitude. Port Meridian is the first. It is fictional, so it sits in open Atlantic water (30°N 40°W) where it cannot collide with a real place; later hand-built areas can replace real districts where they stand. Missions can be placed in either: by local position inside a hand-built city, or by latitude and longitude anywhere.

How it works and how to set it up: [EARTH.md](EARTH.md).

## Port Meridian

Roughly 6 km by 5 km, split by the Wren River, with Meridian Bay to the south. Four bridges cross the river.

| District | Character | Streets |
|---|---|---|
| Ironworks | Container terminals, dry docks, warehouses | Wide, sparse, trucks |
| The Narrows | Canal-side terraces, where Tolly's crew is based | Tight, busy |
| Lantern Quarter | Old town: bars, markets, crooked lanes | Dense, slow, irregular |
| Saltgrass | Flat residential blocks | Regular, calm |
| Northgate | Retail parks, depots, the ring road | Open, fast |
| Breakwater | Boardwalk, hotels, Brass Ring territory | Busy, tourist traffic |
| Meridian Heights | Downtown towers, the money | Gridded, heavy traffic |
| Halfmoon | Residential, hospital, boat sheds | Regular |
| Cinder Hill | Hillside villas, the Commissioner's home | Winding, empty |

The map in `Docs/map.svg` is generated from the same data the game loads. Districts are rectangles for now; that is a blockout decision, not the intended final shape.

## Cast

- **Ines Varga**: the player. Former harbor pilot. Knows the port's schedules and blind spots.
- **Tolly Reyne**: runs a smuggling crew out of a lockup in the Narrows. Gives the early work.
- **Odile Marsh**: owns Pier Nine Motors in Breakwater. Moves cars, cuts seals, knows everyone.
- **Aldous Finch-Mercer**: Harbor Commissioner. Lives on Cinder Hill. Signed the inquiry.
- **The Brass Ring**: the syndicate that runs Breakwater and pays the Commissioner.

## Systems

### Heat (police attention)

- Crimes have a point value (trespass 2, vehicle theft 12, gunfire 22, robbery 30, assaulting an officer 60).
- Unwitnessed crimes do nothing. A police witness adds heat at once. Civilian witnesses report after about 7 seconds, sooner with more witnesses; scare them off or leave and the report can be lost.
- Points map to heat levels 0 to 5. Higher levels bring more units (1, 2, 3, 5, 7).
- **Pursuit** while police can see you. **Search** once they cannot: a zone grows from your last known position and an evade timer runs, twice as fast once you are outside the zone. Being seen again resets it.
- A report phoned in by a civilian sends police to where the crime happened, not to you.

### Traffic

About a hundred cars are simulated in a ring around the player. They follow lanes, keep a time gap to the car ahead, stop at red lights, slow for corners, yield at merges, and brake for the player's car and for police. Gunfire nearby makes drivers run lights and flee. Density follows the district, the time of day and the weather.

Ambient cars are moved along their lanes without physics. That is cheap and looks right at a distance, but a parked ambient car that the player rams does not react yet (see the roadmap).

### Police

Dispatch decides how many units respond, spawns them out of sight on the road network, and gives each one a route: toward the player in a pursuit, sweeping random points inside the search zone otherwise. In Unreal each unit is a physics vehicle steered by the AI driver, and line-of-sight is a real trace from each unit to the player, shortened by fog and rain.

### Missions

A mission is a list of objectives completed in order: go to a place, get in a tagged vehicle, deliver it, collect or eliminate a number of tagged things, lose the police, wait. Objectives can carry a time limit and a heat cap. Missions fail if a named vehicle is destroyed or the player goes down. The first six, in `Content/Data/missions.json`:

| Mission | From | What happens |
|---|---|---|
| Slack Water | Tolly | Move a van from the dry dock to the lockup without drawing heat |
| Paper Trail | Tolly | Take three Brass Ring ledgers, lose the police, bring them back |
| Night Ferry | Odile | Steal a coupe downtown, lose the police, deliver it within five minutes |
| Brass Tacks | Tolly | Clear a Brass Ring warehouse and find a shipping manifest |
| Customs | Odile | Hold the Customs Yard, then drive the truck across the city |
| High Water | Ines | Get into the Commissioner's villa, take the inquiry file, meet Tolly |

### Money

Missions pay cash. Shops sell weapons, ammunition, armour and vehicle work. Five properties can be bought; each pays income once per in-game day and takes 10 to 40 days to pay for itself.

### Bystanders

Each pedestrian has two traits, boldness and civic-mindedness. When something happens nearby the core decides whether they ignore it, watch, cower, flee or report it, based on what it was, how close, whether they could see it, and who they are. Movement and animation are the engine's job.

### Time and weather

One in-game day lasts 48 real minutes. Weather changes hourly with weighted transitions (overcast and rain dominate). Roads take about ten game minutes to get wet and an hour to dry.

## Tuning

All the numbers above are in `Content/Data/tuning.json` and can be changed without recompiling. `ut_sim` prints traffic volume, average speed, heat and police numbers per simulated minute, which is the quickest way to see the effect of a change.
