# Ins Utilities Linux offset audit

## Exact target binaries

| Binary | SHA-256 |
|---|---|
| `insurgency/bin/server_i486.so` | `7afe49221a6f15d583261d9f019b706d88160ad67e6ca1141f5990f7152ffe70` |
| `bin/engine_i686.so` | `bdd6250284924ec8fb7cc264c14fb6e796d53444489d6d771d1a6378cf1a03cf` |

`srcds_run` selects `engine_i686.so`; the adjacent `engine_i486.so` is not the
engine used by this server launch path.

## Verified values

| Owner / purpose | Value |
|---|---:|
| `CGameEventManager::FireEvent` vtable slot | `8` |
| Loaded `engine_i686.so` FireEvent RVA | `0x1CD920` |
| `CPlayTeam::m_iTeamNum` | `0x348` / 840 |
| `CPlayTeam::m_flStartReinforcementTime` | `0x3BC` / 956 |
| `CPlayTeam::m_bEmergencyReinforcement` | `0x3C0` / 960 |
| `CPlayTeam::m_iReinforcementsLeft` | `0x3C8` / 968 |
| `CBaseCombatWeapon::m_hOwner` | `0x468` / 1128 |
| `CBaseEntity::m_spawnflags` | `0xA0` / 160 |
| `CBaseToggle::m_flWait` | `0x34C` / 844 |
| `CBasePropDoor::m_flAutoReturnDelay` | `0x5D8` / 1496 |
| RPG speed signature RVA | `0x6DB0E3` |
| RPG speed immediate RVA / stock value | `0x6DB0E7` / 3000.0 (`0x453B8000`) |
| RPG thrust signature RVA | `0x6DB196` |
| RPG thrust immediate RVA / stock value | `0x6DB19A` / 1000.0 (`0x447A0000`) |

## Sudden-death state

The sudden-death failsafe reads the exported `g_pGameRules` pointer and the
verified `CINSRules` / `CRunningMode` layout without modifying game memory:

| Owner / purpose | Value |
|---|---:|
| `CINSRules::m_iMode` | `0xD8` / 216 |
| `CINSRules::m_pModes` | `0xE0` / 224 |
| `CRunningMode::m_bRoundExtended` | `0x3C` / 60 |

The Linux resolver uses the exported `g_pGameRules` symbol. The state byte is
read only while the server is simulating; if the symbol or verified layout is
unavailable, the failsafe disables itself and logs one warning.

Both RPG signatures occur exactly once in the executable server segment.
