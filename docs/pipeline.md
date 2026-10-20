# The pipeline

How a frame gets from the game to the screen. Three views: the layers a draw call passes through, the GPU work
of one frame in order, and the per-frame state (time of day, weather, darkness) the passes read. The diagrams are
Mermaid (GitHub draws them). Details: docs/frame.md (the game's side), docs/rvk.md (the renderer),
docs/native.md (the native ports), docs/architecture.md (what must stay binary-identical).

## 1. The layers

The game never talks to Vulkan. Every draw comes through `randy31.dll` (ours), as Direct3D 7 calls, onto a renderer
thread.

```mermaid
flowchart TB
    subgraph game["Game thread"]
        AO["AnarchyOnline.exe<br/>GUI.dll, Gamecode.dll"]
        DS["DisplaySystem.dll<br/>~120 visual classes: ground, rooms, sky,<br/>water, effects - each draws itself"]
        subgraph proxy["randy31.dll (ao-vk) - proxy/"]
            EX["771 exports<br/>(exports.gen.cpp)"]
            NAT["native ports - proxy/native/<br/>HookEntry / HookSlot, [Native] modes"]
            ORIG["randy31_orig.dll<br/>(the game's renderer, unported parts)"]
            DD["DirectDraw / D3D7 COM objects on rvk<br/>proxy/ddraw/: rvk_ddraw, rvk_device, rvk_surface"]
            SET["settings table, ini, console<br/>rvk_settings.cpp, rvk_console.cpp"]
        end
        TD["rvk::ThreadedDevice<br/>records each call into a queue"]
    end
    subgraph render["Render thread"]
        DEV["rvk::Device<br/>IDirect3DDevice7-shaped, one uber-shader (ffp.*),<br/>all state dynamic; the features as passes round it"]
        VK["Vulkan 1.3"]
    end
    AO --> DS
    AO --> EX
    DS --> EX
    EX -->|ported| NAT
    EX -->|not yet| ORIG
    NAT --> DD
    ORIG -->|"its D3D7 / DirectDraw calls"| DD
    DD --> TD
    SET -->|"Set..., SetKnob"| TD
    TD -->|"Enqueue: lambdas + state lists"| DEV
    DEV --> VK
```

## 2. One frame on the GPU

A frame is up to five command buffers, submitted in this order. The scene's commands are split at its depth clear
so that the depth pre-pass and the cloud march run before the scene's draws (`prepass.cpp` FrameCommands).

```mermaid
flowchart TB
    UP["upload<br/>textures, GPU skinning's bones"] --> A
    A["mainA<br/>the frame's start, up to the scene's depth clear"] --> PRE
    PRE["prepass<br/>opaque draws' depth only (RVK_Prepass)"] --> CL
    CL["clouds<br/>the cloud layer's march, its shadow, sun visibility"] --> B

    subgraph B["mainB"]
        direction TB
        SCENE["the game's draws, in its render lists 0-7<br/>through the uber-shader into the HDR scene"]
        INS["swapped in or added on the way:<br/>our sky pass in place of the dome - native ground<br/>for AnarchyGround_t - grass tiles at the ground<br/>- leaves before their canopy - our water for<br/>VisualLiquid_t - placed objects - particles"]
        SCENE --- INS
        SCENE --> OVER["over the scene: ground grass field, smoke,<br/>storm debris, rain"]
        OVER --> POST["post passes, each optional:<br/>bloom - AO - GI - environment probe -<br/>volumetric light - sand fog - SSR - contact shadows"]
        POST --> LUMA["land brightness measured (scene_luma.comp:<br/>RVK_Darkness's floor, read a frame later)"]
        LUMA --> DOF["depth of field"]
        DOF --> TM["tone mapping: haze and air, storm air,<br/>rain layer, night darkening, roll-off, grade, LUTs"]
        TM --> AA["TAA + sharpen, or FXAA - motion blur"]
        AA --> UI["the interface (GUI.dll's draws,<br/>or its cached layer at RVK_UiRate)"]
        UI --> ENV["UpdateEnvironment (CPU): sun clock,<br/>conditions, darkness, moon, mist, sky model,<br/>atmosphere tables, terrain sun"]
        ENV --> SH["sun or moon shadow map - point light<br/>shadows (for the next frame's draws)"]
        SH --> BL["blit to the swapchain - console"]
    end
    B --> PRES["submit, present"]
```

What runs where:

| Stage | Code |
| --- | --- |
| Draws, constants, frame lights | `rvk/draw.cpp`, `shaders/ffp*.glsl` |
| Depth pre-pass | `rvk/prepass.cpp` |
| Sky, stars, sun clock, conditions | `rvk/sky.cpp`, `rvk/atmosphere.cpp`, `shaders/sky_glow.frag`, `atmos_*.frag` |
| Clouds | `rvk/clouds.cpp`, `shaders/cloud_layer.frag` |
| Native ground, rocks, pebbles | `rvk/native_ground.cpp`, `ground_clutter.cpp`, `ground_pebbles.cpp`, `shaders/terrain.glsl` |
| Grass, leaves, water | `rvk/grass.cpp`, `leaves.cpp`, `water.cpp` |
| Rain, storms, wind, sand | `rvk/rain.cpp`, `wind.cpp`, `smoke.cpp`, `sand_map.cpp` |
| Post passes, tone mapping | `rvk/hdr.cpp`, `rvk/grading.cpp`, `shaders/tonemap.frag` |
| Darkness | `rvk/darkness.cpp`, `shaders/scene_luma.comp` |
| Moon, sun shadows, point shadows | `rvk/night.cpp`, `shadow.cpp`, `pointshadow.cpp` |
| Interface layer | `rvk/interface.cpp` |
| Present, frame start and end | `rvk/device.cpp` |

## 3. The world's state each frame

Once a frame, at its end, `UpdateEnvironment` brings the time of day and the weather up to date, in order. Each step
reads what the ones before it set. The draws and passes of the next frame read the results directly, or through the
frame lights (`FrameLights`, `shaders/frame_lights.glsl`).

```mermaid
flowchart LR
    GAME["the game's light, fog,<br/>ambient, sky draws"] --> CLOCK
    CLOCK["UpdateSunClock<br/>the true sun eased"] --> COND
    COND["UpdateConditions<br/>m_cond: dusk phases, night,<br/>moon's, clouds', stars' nights,<br/>grade (NightAmount)"] --> DARK
    SETD["RVK_Darkness"] --> DARK
    LUMA["land brightness<br/>(last frame)"] --> DARK
    DARK["UpdateDarkness<br/>m_dark: tone, sky, clouds,<br/>moon, storm, floor gain"] --> MOON
    MOON["UpdateMoon<br/>the moon's light"] --> MIST["UpdateMist"]
    MIST --> SKY["UpdateSkyModel<br/>twilight, low sun's tint"]
    SKY --> ATM["UpdateAtmosphere<br/>our sky's tables"]
    ATM --> TS["UpdateTerrainSun"]
    WEATHER["weather: StormSky, CloudCover<br/>(read live: StormDeck,<br/>StormFull, ClearSky)"] -.-> DARK
    WEATHER -.-> SKY
    TS --> FL["FrameLights (FL.phase, FL.cond, FL.moon, ...)<br/>and the passes' own parameters"]
    COND --> FL
    DARK --> TONE["tone mapping: night darkening,<br/>exposure x floor gain"]
```

The rule since the October 2026 audit (docs/audit-2026-10.md): a feature reads the world's state from `m_cond` and the
weather functions. It doesn't work out its own "how much is it night"; a new ramp goes into `Conditions` with a name.
