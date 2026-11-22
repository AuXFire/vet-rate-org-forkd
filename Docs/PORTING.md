# From clone to driving around Port Meridian

The Unreal code in `Source/Undertow/Game` has **never been compiled**. It was written carefully against the Unreal 5 API and passes a static checker, but the first build will probably report errors. Step 3 says what to do with them.

## 1. Install

- **Unreal Engine 5.4 or later** from the Epic Games Launcher.
- **Windows**: Visual Studio 2022 with the "Game development with C++" workload. **macOS**: Xcode.

## 2. Get the project

```bash
git clone --branch undertow-game <this repository> Undertow
```

Right-click `Undertow.uproject` and choose **Generate Visual Studio project files** (Windows) or **Services > Generate Xcode Project** (macOS). The project is not pinned to an engine version, so you will be asked which installed engine to use.

## 3. Build

Open `Undertow.sln`, select **Development Editor**, build.

If it fails, the errors will almost certainly be in `Source/Undertow/Game`. The simulation in `Source/Undertow/Sim` already compiles cleanly under strict warnings on two compilers. Send the full error output back and they can be fixed quickly. The most likely causes:

| Area | Why it might fail |
|---|---|
| Header locations | Some Unreal headers move between engine versions |
| `UndertowVehicle`, `UndertowAIDriverComponent` | Chaos vehicle input calls differ slightly across versions |
| `UndertowTrafficManager` | AI-driven vehicles rely on `SpawnDefaultController()` so Chaos accepts inputs. If police cars sit still, this is the first place to look |
| Warnings treated as errors in `Sim` | MSVC has warnings gcc and clang do not |

## 4. Make a level

1. **File > New Level > Basic**. Save it as `Content/Maps/PortMeridian`.
2. In **Place Actors**, search for **Undertow City Blockout** and drag one to the origin.
3. Select it and press **Rebuild** in the Details panel. The city appears as grey boxes: ground, roads, buildings, water. It takes a few seconds.
4. Make sure the level has a **Player Start** (anywhere; the game mode moves the player to Tolly's lockup).
5. In **Project Settings > Maps & Modes**, set this level as the Editor Startup Map and Game Default Map.

The level is oriented like `Docs/map.svg` with X pointing east and Y pointing south, the same convention Cesium uses, so the city and a streamed Earth agree.

## 5. Input

Create these under `Content/Input` (right-click > Input):

| Input Action | Value type | Suggested keys |
|---|---|---|
| `IA_Move` | Axis2D | WASD, left stick |
| `IA_Look` | Axis2D | Mouse, right stick |
| `IA_Jump` | Digital | Space |
| `IA_Sprint` | Digital | Left Shift |
| `IA_Interact` | Digital | F |
| `IA_Fire` | Digital | Left mouse |
| `IA_Throttle` | Axis1D | W, right trigger |
| `IA_Brake` | Axis1D | S, left trigger |
| `IA_Steer` | Axis1D | A (negated) and D, left stick X |
| `IA_Handbrake` | Digital | Space |
| `IA_VehicleLook` | Axis2D | Mouse, right stick |
| `IA_Exit` | Digital | F |

Then two Input Mapping Contexts: `IMC_OnFoot` (the first six) and `IMC_Vehicle` (the last six). Each pawn installs its own context when the player takes control of it.

## 6. Blueprints

The C++ classes carry the logic; Blueprints supply meshes and assets.

1. **Add starter content**: in the Content Browser, **Add > Add Feature or Content Pack**, add **Third Person** and **Vehicle**. These give you a rigged mannequin with animations and a working car.
2. **`BP_Player`**, parent `UndertowCharacter`. Set the mesh and animation Blueprint from the Third Person pack. Assign `IMC_OnFoot` and the six on-foot actions.
3. **`BP_Car`**: open the Vehicle pack's sports car pawn Blueprint and use **File > Reparent Blueprint** to `UndertowVehicle`. That keeps its mesh, wheels and engine setup. Delete its own input graph, then assign `IMC_Vehicle` and the six vehicle actions. Set **Vehicle Tag** (for example `coupe`). Make copies with tags `van` and `truck` for the missions.
4. **`BP_PoliceCar`**, a child of `BP_Car`. Add an **Undertow AI Driver** component.
5. **`BP_AmbientCar`**: a plain Actor Blueprint with just a static car mesh. One is enough to start.
6. **`BP_TrafficManager`**, parent `UndertowTrafficManager`. Add `BP_AmbientCar` to **Ambient Vehicle Classes** and set **Police Vehicle Class** to `BP_PoliceCar`.
7. **`BP_GameMode`**, parent `UndertowGameMode`. Default Pawn Class `BP_Player`, Traffic Manager Class `BP_TrafficManager`. Select it in **Project Settings > Maps & Modes**.
8. **`WBP_HUD`**, a Widget Blueprint with parent `UndertowHUDWidget`. Add text for heat, cash, clock and objective, implement the four events, and add it to the viewport from `BP_Player`'s BeginPlay.
9. Place a few `BP_Car` instances near the start, and an **Undertow Mission Marker** with Mission Id `m01_slack_water` near the player.

## 7. Check it works

Press Play and go through this list. Each item exercises one part of the bridge.

- [ ] You spawn in the Narrows and can walk, sprint and jump
- [ ] Ambient cars drive past in their lanes and stop at some junctions
- [ ] F next to a car gets you in; it drives; F gets you out
- [ ] Taking a car you do not own raises heat a few seconds later (the HUD heat event fires)
- [ ] A police car arrives, follows you, and line of sight matters: break it and the evade bar fills
- [ ] Firing a weapon makes nearby ambient cars speed away
- [ ] Walking into the mission marker shows the first objective
- [ ] The clock advances and the weather name changes over a few minutes

The Output Log line `Simulation ready: Port Meridian, 2083 junctions, 6 missions` confirms the data loaded. If it is missing, look for `LogUndertow` errors above it.

## 8. The real Earth

Port Meridian is a test city. To play on the real planet at full scale, with streamed terrain and real roads, follow [EARTH.md](EARTH.md) once the steps above work.

## 9. Where real quality comes from

The blockout is for testing. For the look and feel you are after, build on what Epic gives away:

- **City Sample** (the *Matrix Awakens* city): buildings, vehicles, crowds and traffic at production quality, and the best reference for large-city streaming.
- **Game Animation Sample**: motion-matched locomotion and traversal, far ahead of the starter mannequin.
- **MetaHuman**: characters.
- **Fab / Megascans**: environment materials and props.

Check each asset's licence terms on its Fab page before shipping anything with it.
