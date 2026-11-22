# Roadmap

## What "GTA quality" would take

The games this is inspired by are made by well over a thousand people across many years, and most of that effort is content: a hand-built city, thousands of animations, voiced performances, vehicle handling tuned car by car, and years of testing. Code is the smaller part.

What one person with these tools can realistically reach is a **vertical slice**: a few city blocks at high visual quality, on-foot and driving that feel good, working police and traffic, and two or three complete missions. That is a real, showable game, and the foundation in this repository is built to get there. The whole 6 km city at that quality is a team-sized job.

## Where things stand

**Tested and working (outside the engine)**

- Heat, witnesses, search zones and evading
- Road network, routing, lane-following traffic with signals, merging, obstacles, panic
- AI driver that follows a path, slows for corners, stops at the end
- Police dispatch by heat level, pursuit and search behaviour
- Data-driven missions, economy, properties, save and load, clock and weather
- Bystander reaction decisions
- The generated city: connected roads, bridges, building masses, locations
- The whole Earth at full scale: real-world coordinates, road tiles streamed around the player, moving the origin as the player travels, real roads converted from OpenStreetMap (see [EARTH.md](EARTH.md))

**Written, not yet compiled or run**

- Everything in `Source/Undertow/Game`: the subsystem, character, vehicle, AI driver component, traffic and police manager, weapon, HUD base, blockout actor, mission actors

**Not started**

- All art: characters, vehicles, buildings, props, materials, lighting, effects
- Animation: locomotion, entering and leaving vehicles, combat, cover
- Audio: engines, weapons, city ambience, radio, voice
- Pedestrians in the world (the decisions exist, the walking people do not)
- Enemy combat AI, cover, melee
- Minimap, GPS route line, menus, shops and property UI
- Cutscenes and dialogue
- Ambient cars that react physically when hit
- Lane changing and overtaking in traffic, give-way at unsignalled crossings
- Interiors, boats, motorcycles, aircraft
- For the Earth: road tiles for more than the Monaco sample, pedestrians and buildings from map data, cars at the right height in tunnels and on stacked roads, background tile loading
- Performance work, console targets

## Suggested order

### A note on the full Earth

The planet-sized world changes where the effort goes. Streamed scenery covers everywhere at mapping-app quality, so the map is no longer the bottleneck, and the visual quality at street level is capped by the streaming source. The plan below still holds: get it running in Port Meridian, make driving and police feel right, then decide which real places deserve hand-built detail on top of the streamed base.

### Milestone 1: it runs (about a week)

Get through [PORTING.md](PORTING.md): compile, fix what the first build reports, rebuild the blockout, and tick every item on the check list. Nothing else is worth doing until walking, driving, traffic and a police chase work in the grey-box city.

### Milestone 2: it feels right (2 to 4 weeks)

- Replace the starter mannequin locomotion with the Game Animation Sample
- Tune one car until driving it is fun: this matters more than any feature
- Entering and leaving vehicles with animation instead of a teleport
- Aiming, recoil, hit reactions, a health and armour HUD
- Minimap with a GPS line (the route already comes from `RoadGraph::findPath`)

### Milestone 3: the city is alive (4 to 8 weeks)

- Pedestrians: spawn to `Simulation::pedestrianBudget()`, walk the pavements, react with `Crowd::react()`
- Ambient cars become physics vehicles when the player gets close or hits them
- Police: roadblocks at higher heat, on-foot officers, arrests
- Enemy AI for the two combat missions

### Milestone 4: one district, properly (2 to 4 months)

Pick the Narrows and Lantern Quarter. Replace the grey boxes with real streets using City Sample and Megascans assets, light it for rain and night, add audio. Make missions one to three playable start to finish with dialogue.

### Milestone 5: decide

With a polished slice you can judge how the game plays, show it to people, and decide whether to grow a team for the rest of the city or ship a smaller game.

## Risks worth knowing now

- **The first compile.** The engine-side code is unverified. Budget time for it.
- **Driving feel.** Chaos vehicles take real tuning to feel good, and it is the core of the game.
- **Scope.** Every system in the "not started" list is weeks of work. Finish the slice before widening it.
- **Asset licences.** Free samples have terms. Read them before publishing.
- **Streaming and map licences.** A full-Earth game depends on a terrain and imagery service and on OpenStreetMap. Each has terms on attribution, caching and commercial use.
