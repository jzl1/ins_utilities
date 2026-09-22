# ins_utilities

A lightweight C++ Valve Server Plugin (VSP) providing quality-of-life enhancements, gameplay mechanics, and server stability fixes for *Insurgency: Modern Infantry Combat* (Source 2006 / Episode 1 Engine, SRCDS build 4044). It loads directly from `addons/ins_utilities.vdf` and does not require MetaMod:Source.

---

## Key Features

* **Door Enhancements & Smoothing:** Proximity auto-opening for bots and players (`door_auto_open`), adjustable opening/closing speed (`door_speed`), persistent open doors (`keep_doors_open`), and network snapshot interpolation for stutter-free door animations (`door_smoothness`).
* **Aim + `+use` Weapon Swap:** Pick up and swap dropped weapons on the ground by aiming and pressing `+use` (F key) with a natural toss mechanic (`weapon_swap`).
* **Server Stability & Map Watchdogs:** Automatic map rotation failsafes for endless rounds (`force_map_switch`), Sudden Death rotations (`sudden_death_map_switch`), and idle server recycling (`force_map_switch_idle`).
* **Battlefield Cleanup:** Configurable periodic cleanup of dropped weapons, ragdolls, and decals to optimize server physics performance.
* **Combat & Gameplay Utilities:** Customizable RPG-7 projectile velocity (`rpg_speed`), instant reinforcement respawns (`instant_respawn`), and killfeed muting (`mute_killfeed`).

---

## Installation Instructions

### 1. Windows Dedicated Server (`srcds.exe`)
* Copy `ins_utilities.dll` and `ins_utilities.vdf` into your server's `insurgency/addons/` directory.

### 2. Linux Dedicated Server (`srcds_run` / `srcds_i486`)
* Copy `ins_utilities_i486.so` and `ins_utilities.vdf` into your server's `insurgency/addons/` directory.

### 3. Start or Restart your Server
* The Source engine will automatically detect and load the plugin on startup.
* Verify startup in the server console:
  ```
  [ins_utilities] loaded
  ```

---

## Configuration & Usage

All settings are managed internally via the `ins_util` console command to avoid engine autocomplete limits:

| Command | Description |
|---|---|
| `ins_util list` | Display all available settings, cvars, and descriptions. |
| `ins_util get <name>` | Check the current value of a setting. |
| `ins_util set <name> <value>` | Modify any utility setting live during gameplay. |
| `ins_util version` | Display plugin version and build timestamp. |

### Quick Start (`server.cfg`)

Add your preferred settings to `insurgency/cfg/server.cfg`:

```cfg
// Door mechanics
ins_util set door_smoothness 1
ins_util set door_auto_open 1
ins_util set door_speed 200
ins_util set keep_doors_open 1

// Weapon swap (+use on dropped weapons)
ins_util set weapon_swap 1
ins_util set weapon_swap_range 100
```
