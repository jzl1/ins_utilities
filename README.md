# ins_utilities

A lightweight Plugin providing quality-of-life enhancements, gameplay mechanics, and server stability fixes for *Insurgency: Modern Infantry Combat* (Source 2006 / Episode 1 Engine, SRCDS build 4044).

## Quickstart

1. Download the latest release [here](https://github.com/jzl1/ins_utilities/releases/latest)
2. Open the downloaded archive (`.zip` for Windows, `.tar.gz` for Linux).
3. Copy or drag the `addons` folder directly into `Source SDK Base/insurgency/` (merge folders when prompted).

---

## Configuration & Usage

All settings are managed internally via the `ins_util` console command to avoid engine autocomplete limits:

| Command | Description |
|---|---|
| `ins_util list` | Display all available settings, cvars, and descriptions. |
| `ins_util get <name>` | Check the current value of a setting. |
| `ins_util set <name> <value>` | Modify any utility setting live during gameplay. |
| `ins_util version` | Display plugin version and build timestamp. |

See [cvars.txt](cvars.txt) for the full list of settings, their default values, and descriptions.

---

## License

Released under the [GNU General Public License v3.0](LICENSE).

Copyright (c) 2026 jzl1

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, version 3 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program. If not, see <https://www.gnu.org/licenses/>.

### What this means in practice

You are free to run, study, modify and redistribute ins_utilities, including on
your own server and including as part of a paid server hosting service. If you
distribute a modified version, you must make its complete source code available
under the same license, and you may not add restrictions of your own on top.
