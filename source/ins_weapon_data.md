# INS:MIC weapon data — extracted from the game files

Decrypted from `scripts/weapons/*.ctx` and `scripts/pclasses/*.ctx` (ICE
level-1, key `DrA5e3EB`, pulled from `GetEncryptionKey` in the server binary),
cross-referenced with `clipdata.ctx`, `ammodata.ctx`, and `weapondata.ctx`.

## Primary weapons

| classname | mag | carry | caliber | fire modes | RPM | spread° | recoil× | shake× |
|---|---:|---:|---|---|---:|---:|---:|---:|
| weapon_m16a4  | 30 | 6 | 5.56 NATO | Semi / **Burst-3** | 700¹ | 0.165 | 2.7 | 0.75 |
| weapon_m16m203| 30 | 6 | 5.56 NATO | Semi / **Burst-3** | 700¹ | 0.165 | 2.5 | 0.75 |
| weapon_m4     | 30 | 6 | 5.56 NATO | Semi / Auto | 700 | 0.140 | 2.1 | 0.4 |
| weapon_m4med  | 30 | 6 | 5.56 NATO | Semi / Auto | 700 | 0.155 | 1.8 | 0.4 |
| weapon_m14    | 20 | 6 | 7.62 NATO | Semi | 290 | 0.095 | 2.5 | 1.0 |
| weapon_ak47   | 30 | 6 | 7.62×39 | Semi / Auto | 600 | 0.185 | 2.5 | 0.5 |
| weapon_ak74   | 30 | —² | 5.45×39 | Semi / Auto | 600 | 0.185 | 1.0 | 1.0 |
| weapon_aks74u | 30 | 6 | 7.62×39³ | Semi / Auto | 650 | 0.200 | 2.2 | 0.5 |
| weapon_sks    | 10 | 12 | 7.62×39 | Semi | 170 | 0.150 | 2.5 | 0.4 |
| weapon_fnfal  | 20 | 6 | 7.62 NATO | Semi | 650 | 0.175 | 2.5 | 0.5 |
| weapon_rpk    | 75 | 4 | 7.62×39 | Semi / Auto | 600 | 0.230 | 3.5 | 0.3 |
| weapon_m249   | 200 | 4 | 5.56 NATO | Auto | 725 | 0.230 | 3.5 | 0.2 |
| weapon_svd    | 10 | 6 | .303 | Semi | 170 | 0.095 | 2.5 | 1.0 |
| weapon_l42a1  | 10 | 6 | .303 | Semi (bolt) | ~55⁵ | 0.095 | 2.5 | 0.5 |
| weapon_m1014  | 7 | 6 | 12ga buck | Semi | 200 | 1.60 | 5.0 | 0.5 |
| weapon_toz    | 6 | 6 | 12ga buck | Semi (pump) | 200 | 1.60 | 5.0 | 0.5 |

*RPM = the game's `cycle_time` (rounds/min = mechanical cyclic rate). ms between
rounds = 60000 / RPM (ak47 600 rpm → 100 ms/round). For semi weapons this is the
max cap; real rate is trigger-limited.*

## Sidearms

| classname | mag | carry | caliber | fire modes | recoil× | shake× |
|---|---:|---:|---|---|---:|---:|
| weapon_m9      | 15 | 2–3 | 9mm NATO | Semi | 4.0 | 2.0 |
| weapon_makarov | 10 | 2–5 | 9mm NATO | Semi | 4.0 | 3.0 |
| weapon_1911    | 7  | —²  | .45     | Semi | 5.0 | 2.5 |

## Launcher

| classname | mag | carry | projectile | recoil× |
|---|---:|---:|---|---:|
| weapon_rpg7 | 1 (OG7V) | — | 200 dmg / 10 radius | 10.0 |

### Notes
1. **Burst / M16**: firemodes bitfield is Semi=1, Burst=2, Auto=4. M16A4/M203 =
   3 (Semi | 3-round-burst); the burst uses `burst_cycle_time` 700 ms. Every
   Auto weapon = 5 (Semi|Auto) except the M249 = 4 (Auto only).
2. **carry "—"**: not present in any of the 14 stock class kits, so bots never
   spawn with it in normal play (AK-74 ≈ AK-47 platform; 1911 ≈ 2 like other
   pistols if ever kitted).
3. **AKS-74U caliber**: the game data points its `clip_type` at `CLIP_AK47`
   (7.62×39, 30 rnd) — that's the mod's actual value, not a typo on my side.
5. **Bolt (L42A1)**: no cycle field in the script; the ~55 rpm is a practical
   bolt-cycling estimate, not a game value. Pistols likewise have no cycle field
   (semi, click-limited) so no RPM is listed for them.

### "carry" = magazines the kit spawns with (loaded one included)
So backup mags = carry − 1. Example: M16A4 rifleman carries 6 mags = 1 loaded +
5 spare = 180 rounds total. Sniper's SKS carries 12.

### Recoil model (global, in weapondata.ctx — the recoil× column multiplies this)
Base per-shot kick: pitch (up) `recoil_x` −3.0…−1.0, yaw `recoil_y` −1.0…0.0.
Stance/motion multipliers stack on top: ironsights ×0.80, running ×1.75,
crouch ×0.90, crouch+iron ×0.70, prone+iron ×0.50. View-shake base 0.50,
amplified by low-stamina ×3.5, wounded ×2.5, moving ×2.0. Ironsights also
double accuracy (`accuracy_mod_ironsights` 2.0), i.e. roughly halve the spread
cone.
