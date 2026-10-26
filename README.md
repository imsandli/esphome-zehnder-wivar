# Zehnder Wivar BLE bridge for ESPHome [![Made for ESPHome](https://img.shields.io/badge/Made_for-ESPHome-black?logo=esphome)](https://esphome.io)

ESPHome firmware that connects to the Bluetooth remote of a **Zehnder Wivar** electric towel radiator heater and
brings it into Home Assistant: room temperature, setpoints, thermostat, mode, weekly schedule, heating timer,
window detection and the remote's status.

> **Experimental.** Tested with one Wivar II remote ("Steuergerät Modell 1", firmware 0x100411) only. See the
> [disclaimer](#disclaimer) before you use it.

## Features

- Room temperature, target temperature, run mode, window open, room name
- Thermostat, Home and Away setpoints, mode (Automatic / Manual / Antifreeze)
- Heating timer (15 min to 4 h), including timers started on the remote or in the Zehnder app
- Weekly schedule: read it as text and change it from Home Assistant
- Remote clock and daylight-saving dates set once a day
- Battery-friendly: one short Bluetooth connection every 5 minutes; changes go out at once
- Diagnostics: connection status, signal, battery low, errors, firmware, connection count and duration

## Hardware

The bridge runs on any ESP32 with Bluetooth LE that's in range of the remote. The repo ships two device configs:
`wivar-shelly.yaml` for a **Shelly PM Mini Gen3** (what I run now) and `wivar-lcd.yaml` for an Espressif
**ESP32-C3-LCDkit** with a round display and a knob (the earlier bridge). For other boards see
[your own board](#your-own-board).

### Shelly PM Mini Gen3

The Shelly PM Mini Gen3 (S3PM-001PCEU16) is a small power meter for the wall box: an ESP32-C3 with 8 MB flash
and a BL0942 metering chip, but **no relay**. It only measures; the heater's power runs straight through it. In
my setup it sits in the wall box behind the heater's plug and runs this firmware instead of Shelly's, so it is the
bridge to the remote and the heater's power meter in one.

What `wivar-shelly.yaml` adds to the bridge:

- Heater power, energy, voltage and current, mains frequency (BL0942, read every 5 s)
- "Heating": on while the heater draws more than 20 W (`heating_threshold_w`), off 30 s after it stops. The
  remote never reports whether the heater runs; the power meter does.
- Shelly temperature (the board's NTC) and Wi-Fi signal
- Button: short press polls the remote now, hold 3 s starts pairing. The LED shows ESPHome's status.
- Web UI at `http://<device>/` with digest login (`web_username` / `web_password`), entities in groups. It takes
  no firmware uploads (`ota: false`: digest logins can be replayed on plain HTTP); updates go through the encrypted
  native OTA, and the fallback hotspot's page can still upload one.
- Two Wi-Fi networks (`wifi_ssid` preferred, then `wifi_main_ssid`), and the hotspot "Wivar Shelly Fallback" if
  neither can be joined. Networks entered on the hotspot's page are forgotten with the next build.
- Shelly's own partition layout (`MiniPMG3-stock.csv`), so the firmware fits the flash as Shelly left it

| GPIO | Function |
|---|---|
| 0 | Status LED (inverted) |
| 1 | Button (inverted, pull-up) |
| 3 | NTC (board temperature) |
| 6 / 7 | BL0942 UART TX / RX (RX needs the pull-up, otherwise every reply fails its checksum) |

Voltage is calibrated against my electricity meter (`voltage_reference`); current and power keep ESPHome's
defaults, which matched the Shelly firmware's readings of the heater within about 1 %.

I installed ESPHome on it over the air with [free-shelly-ota](https://github.com/oxynatOr/free-shelly-ota).
**Getting ESPHome onto a Shelly is not part of this project and not supported here.** Shelly devices run on mains
voltage: installing one in a wall box is work for a qualified electrician.

> **Warning: Shelly models with a relay.** `wivar-shelly.yaml` is for the PM Mini Gen3 only, which has no relay.
> Don't flash it onto a similar Shelly that has one (Mini 1PM Gen3, Plus 1PM, ...): their pinout and flash layout
> differ, and the config doesn't define the relay, so the heater can end up switched off, or switched by a GPIO
> meant for something else. If you adapt the config to such a model, define the relay explicitly with a safe
> `restore_mode`, and keep in mind that cutting the heater's power works against the remote's thermostat and
> schedule. This bridge never switches the heater's power; it only talks to the remote.

### Only one bridge

Run only one bridge at a time. The remote accepts a single Bluetooth connection, and none while the Zehnder phone
app is connected.

## Installation

Tested with ESPHome 2026.10.0 (pinned in `requirements.txt`, and the configs' `min_version`). The bridge can be
pulled from GitHub into your own config without a checkout ([your own board](#your-own-board)); the device configs
in this repo are built from a checkout, with Python 3:

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
cp secrets.yaml.example secrets.yaml && $EDITOR secrets.yaml
```

`secrets.yaml` holds:

- Wi-Fi and the timezone
- `wivar_mac`: the remote's Bluetooth MAC. Find it with any BLE scanner: the remote advertises as `BasicRF..`
  with Zehnder manufacturer data.
- `wivar_app_uuid`: the identity the bridge registers on the remote when pairing. Any random UUID; keep it, so
  you don't have to pair again.
- API keys (they also encrypt OTA updates) and, for the Shelly, the fallback hotspot and web UI passwords.
  `secrets.yaml.example` shows how to generate them.

### Shelly PM Mini Gen3

Once the Shelly runs ESPHome, build and update it over Wi-Fi:

```bash
.venv/bin/esphome run wivar-shelly.yaml
```

ESPHome finds the device by its name, `wivar-shelly.local` (mDNS), and the upload is encrypted with the API key, so
no other device accepts it. If the name isn't found, give the IP address: `--device 192.168.x.y`.

### ESP32-C3-LCDkit

`wivar-lcd.yaml` adds a display (room temperature gauge, target, room name, status) and the knob: turn for the
backlight, press to refresh, hold 3 s to pair. It needs `wivar_lcd_api_key` in `secrets.yaml`. Flash it over USB
the first time (`.venv/bin/esphome run wivar-lcd.yaml`), later over Wi-Fi.

### Your own board

A config that pulls the bridge from GitHub; it works in the ESPHome dashboard as well. Add `wivar_mac`,
`wivar_app_uuid`, `timezone`, Wi-Fi and an API key to its `secrets.yaml` (see `secrets.yaml.example`):

```yaml
substitutions:
  name: my-wivar
  friendly_name: Wivar
  wivar_mac: !secret wivar_mac
  wivar_nickname: "ESPHome"   # name the bridge registers on the remote (max 32 characters)

esphome:
  name: ${name}
  friendly_name: ${friendly_name}

esp32:
  board: esp32-c3-devkitm-1   # your board
  framework:
    type: esp-idf

packages:
  wivar: github://imsandli/esphome-zehnder-wivar/common/wivar.yaml@main

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret my_api_key   # add to secrets.yaml

ota:
  - platform: esphome

logger:
```

To pin a version, use a tag instead of `main` in the package and set the substitution `wivar_components` to
`github://imsandli/esphome-zehnder-wivar@<tag>`, so the component matches the package. In a checkout, include
`common/wivar.yaml` with `!include` and set `wivar_components: components` instead, as the repo's configs do.
`wivar-lcd.yaml` is a complete example for another board: drop its display, knob and backlight sections.

For the Shelly in the ESPHome dashboard, copy `wivar-shelly.yaml` and `MiniPMG3-stock.csv` and replace its
`!include` and `wivar_components` lines with the GitHub package above.

## Pairing

The remote has to know the bridge before it answers. The remote must already be paired with the heater, and
the Zehnder app must be closed.

1. Start pairing: "Pair Wivar" in Home Assistant, or hold the Shelly's button for 3 s.
2. When the remote's AUTO light flashes, briefly press **MODE on the remote**.
3. "BLE status" shows "Paired", and the readings follow.

A second bridge with the same `wivar_app_uuid` doesn't need to pair again.

**Don't use the pairing button on the heating rod.** It links the rod to the remote by radio, and holding it for
10 s resets the rod to factory settings, which erases the schedule.

## Home Assistant

The ESPHome integration discovers the device; enter the API key from `secrets.yaml`.

| Group | Entities |
|---|---|
| Room | Temperature, Target temperature, Run mode, Window open, Room |
| Control | Thermostat (setpoint of the active mode), Home setpoint (15–28 °C), Away setpoint (10–25 °C), Mode, Refresh now |
| Heating timer | Heat 15 min / 30 min / 45 min / 1 h / 2 h / 4 h, Heat timer stop, Heat timer remaining |
| Schedule | Schedule, Schedule today, Next switch, Schedule write, Schedule edit |
| Heater | Heater overheated, Surface temperature setting (50/65 °C, read only) |
| Diagnostics | BLE status, Remote signal, Remote battery low, Remote error, Remote firmware, Remote firmware untested, BLE connections, BLE connected time, Pair Wivar, Restart |
| Shelly | Heater power / energy / voltage / current, Mains frequency, Heating, Shelly temperature, WiFi signal |

Modes: **Automatic** follows the schedule, **Manual** holds the Home temperature (the Wivar ignores Away in
Manual), **Antifreeze** is the remote's standby at 7 °C.

"Remote battery low" comes from the remote's error flags; it reports no battery level. "Heater overheated"
has never been seen on a Wivar and normally stays off. "Remote firmware untested" turns on when the remote runs
a firmware other than the tested one.

### Schedule

"Schedule" shows the week as text, for example:

```
Mo-Fr: 00:00 Away, 06:20 Home, 06:45 Away; Sa-Su: 00:00 Away, 08:30 Home, 09:00 Away
```

It's read at start, on "Refresh now" and once a day. To change it, call the action
`esphome.<device>_set_schedule` with text in the same format:

```yaml
action: esphome.wivar_shelly_set_schedule
data:
  schedule: "Mo-Fr: 06:20 Home, 06:45 Away; Sa-Su: 08:30 Home, 09:00 Away"
```

- Days you leave out keep their plan.
- A day that doesn't start at 00:00 begins with the mode it ends with.
- `Preheat 06:00 30 min` adds a towel preheat timer.
- The remote holds at most 8 day plans. The bridge reuses, adds and removes plans as needed and names them after
  their days (Weekdays, Weekend, Monday, ...).
- The bridge reads the schedule first, so changes made in the app are kept. It checks the result by reading the
  schedule back, and "Schedule write" shows the outcome. A change cut off by a lost connection continues in the
  next session.

The "Schedule edit" text field (web UI, dashboards) does the same, but holds at most 255 characters; use the
action for longer changes.

## How it behaves

- **Polling:** every 5 minutes, to spare the remote's batteries. Changes from Home Assistant are sent at once.
  Readings become unknown after 20 minutes without a successful poll.
- **Connecting:** the remote advertises only every 5–8 s, so a connection can take up to 40 s. About 1 connect in
  7 fails and is retried after 30 s (up to 3 times). Such failures only show in "BLE status" once all retries
  have failed. Connections are kept at least 20 s apart, because the remote often refuses one right after the
  last.
- **Writes:** a setpoint write always carries all modes, so the bridge reads the current setpoints first in the
  same connection; a change made in the app since the last poll is kept. Setpoint changes are confirmed in the
  same connection. Timers and unconfirmed writes are read back 10 s later. A change made while an earlier one is
  still being written is sent in the next connection.
- **Clock:** the remote runs its schedule on its own clock and doesn't report it. The bridge sets it once a day,
  with the daylight-saving dates (`wivar_sync_clock: "false"` turns this off).
- **Restart:** a change from Home Assistant that hasn't reached the remote yet (normally seconds, at most a few
  minutes while the remote is unreachable) is lost when the bridge restarts or is updated. Pending changes are
  deliberately not stored: replaying one much later, a heating timer for example, could do more harm than good.

### Options

Substitutions in the device config (defaults from `common/wivar.yaml`):

| Substitution | Default | |
|---|---|---|
| `wivar_mac` | (required) | The remote's Bluetooth MAC |
| `wivar_nickname` | (required) | Name the bridge registers on the remote (max 32 characters) |
| `wivar_update_interval` | `5min` | Poll interval |
| `wivar_connect_timeout` | `40s` | Scan and connect limit per session (also the BLE scan duration) |
| `wivar_retry_interval` / `wivar_max_retries` | `30s` / `3` | Retries after a failed poll |
| `wivar_stale_after` | `20min` | Readings older than this become unknown |
| `wivar_min_setpoint` / `wivar_max_setpoint` | `7` / `28` | Setpoint range offered and written (can be narrowed, not widened) |
| `wivar_sync_clock` | `true` | Set the remote's clock once a day |
| `wivar_room_index` | `0` | Which room of the remote to use |
| `wivar_components` | GitHub, `@main` | Where the component comes from; `components` in a checkout |

## Troubleshooting

- **"Wivar not reachable":** the bridge doesn't see the remote. Check the distance; metal (a radiator, a wall
  box) easily blocks the remote's weak signal. Close the Zehnder app and make sure no other bridge is running:
  while the remote is connected to them it stops advertising, which looks the same to the bridge.
- **"Not paired":** pair as described above.
- **Upload can't find the device:** mDNS (`<name>.local`) usually doesn't cross VLANs or subnets. Pass the IP with
  `--device`, or set `use_address` under `wifi:`.
- **Shelly power readings:** `voltage_reference` in `wivar-shelly.yaml` is calibrated for my mains; adjust it to
  your meter.

## Development

Besides the venv from [Installation](#installation), the tests and CI need `g++` and `rsync`; ShellCheck is used when
installed. GitHub Actions runs the full `ci/run.sh` on every push and pull request.

```bash
tests/run.sh                                        # host tests of protocol and session logic (g++, sanitizers)
ci/run.sh --quick                                   # lint, tests, privacy check, config validation
ci/run.sh                                           # the same plus the firmware build
ci/install-hooks.sh                                 # quick CI before commit, full CI before push
.venv/bin/python tests/smoke.py HOST API_KEY        # read-only check of a running bridge
.venv/bin/python tests/device_test.py HOST API_KEY  # full test of a running bridge: writes, then restores
```

- `tests/device_test.py` tests a running bridge end to end: it changes the setpoints, mode, a heating timer and one
  day of the schedule, checks each change and writes the original values back. It switches the heater on for a
  moment (`--no-heat` skips that). It takes its backup from a fresh read of the remote and saves it to a local
  file, refuses to start while a heating timer runs, and leaves the schedule alone if the "Schedule" sensor can't
  restore it (longer than 255 characters). Whatever it can't restore or verify at the end is listed under
  "RECOVERY NEEDED", with the original values. Run it after flashing a change.
- Live tests change something only when its backup is verified to restore it; otherwise they refuse or skip that
  part, and they check the restore by reading the remote back.
- The fake remote in the session tests replies the way the real one was seen to reply (e.g. rooms and run-state
  updates before a SetDayplan confirm, the assigned id in an AddDayPlan confirm, no answer to GetTime). Change it
  only with evidence from a real remote, and note that evidence in docs/PROTOCOL.md.
- The component lives in `components/zehnder_wivar/`: `wivar_proto.*` (messages and framing), `wivar_session.*` and
  `wivar_schedule.*` (session logic and schedule planning, plain C++ tested against a fake remote) and
  `zehnder_wivar.*` (ESPHome glue).
- `docs/PROTOCOL.md` describes the Bluetooth protocol as far as it's known.
- CI uses dummy secrets from `ci/secrets.ci.yaml`. Its privacy stage keeps values from your `secrets.yaml`, MACs,
  private IPs and keys out of commits.
- To capture frames for tests, build with `logger: level: VERBOSE`: the component logs every request (`TX`) and
  reply (`RX`). Login and pairing requests are logged without their body (it contains `wivar_app_uuid`). Replace
  the remote's ID string before committing replies.

## Sources and credits

- The protocol was worked out for interoperability, from the Bluetooth traffic of the official Zehnder app and
  from studying that app, then confirmed against a real remote ([docs/PROTOCOL.md](docs/PROTOCOL.md)). This repo
  contains no Zehnder code, app packages or decompiled sources.
- Shelly PM Mini Gen3 pinout: [devices.esphome.io](https://devices.esphome.io/devices/shelly-pm-mini-gen3/).
  `MiniPMG3-stock.csv` reproduces the partition layout read from Shelly's official 2.0.1 firmware package, so
  builds match the layout already in the device's flash.
- Built on [ESPHome](https://esphome.io/).

## License

[MIT](LICENSE). Compiled firmware also contains ESPHome and its dependencies under their own licenses (ESPHome's C++
runtime is GPLv3).

## Disclaimer

- This is a private hobby project, not affiliated with or endorsed by Zehnder.
- It is **not widely tested**: one heater, one remote, one remote firmware. Other models, firmware versions or
  setups may behave differently or not work at all.
- Most of the code and documentation was **generated with Claude** (Anthropic's AI assistant) and reviewed and
  tested by me only as far as described above.
- Writes change how your heater runs (setpoints, mode, schedule, timers). A bug can leave a room cold, heat it
  longer than intended or overwrite your schedule. Shelly devices work on mains voltage.
- **Use at your own risk.** No warranty, no support.
