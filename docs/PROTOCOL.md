# Zehnder Wivar BLE protocol

What `components/zehnder_wivar` implements, as confirmed against a Wivar II remote
("Steuergerät Modell 1", firmware 0x100411).

## Devices

- The **remote** is the only BLE device. It advertises as `BasicRF<xx>` with manufacturer data
  containing "Zehnder", only every 5–8 s, and accepts one connection at a time. While connected (e.g. to the
  phone app) it stops advertising: a scan can't tell that from a remote out of range (tested 2026-10-25, a 40 s
  scan with the app open found nothing; 3 s after closing the app it was found).
- The **heating rod** has no BLE. It talks to the remote over a separate radio link (the remote's
  name suggests TI BasicRF, 2.4 GHz IEEE 802.15.4) and is not reachable from here.
- The remote measures the room temperature itself.

## GATT

| Role | UUID |
|---|---|
| Service | `2456e1b9-26e2-8f83-e744-f34f01e9d701` |
| Write (requests) | `2456e1b9-26e2-8f83-e744-f34f01e9d702` |
| Read / notify (replies) | `2456e1b9-26e2-8f83-e744-f34f01e9d703` |

Requests are written in 20-byte chunks with write-with-response. Replies arrive as notifications on
the read characteristic. The remote pushes one chunk, and the next one after each **read** of the
characteristic (the read value itself is stale). It sometimes doesn't push the first chunk at all,
so the client polls with a read every second while waiting.

## Framing

```
[4 bytes BE] total = op_len + 2 + body_len
[2 bytes BE] op_len
[op_len]     GatewayOperation { type(1), result(2), resultDescription(3), reference(4) }
[body_len]   request / reply message
```

All messages are protobuf (proto2). `result` 0 = OK, 5 = NOT_PAIRED, 10 = TIMEOUT.

## Session

1. **SetAddress** (1 → 51) `{ uuid(1): app UUID }`: the app's login. Some requests only answer after it.
2. Optional writes (below).
3. **GetRunState** (240 → 241): `cRunstate { runMode(1), currentTempMode(2), followingSchedule(3),
   standbyModeActive(7), deviceErrors(11) }`. runMode 1 Automatic, 2 Manual, 3 Antifreeze, 4 Off;
   temp mode 0 Home Awake, 1 Home Asleep, 2 Away, 3 Antifreeze, 4 Override. The bridge reads it before the
   rooms, so that a thermostat change queued meanwhile goes to the mode active now.
4. **GetRooms** (194 → 195): room record, see below.
5. **Version** (18 → 68): `systemVersion(1)`.

Unsolicited **RoomsUpdateIndication** (196) frames can arrive at any time.

## Room record (`cRoom`)

| Field | Meaning |
|---|---|
| 1 | id |
| 2 | name |
| 4 | target temperature (0.1 °C) |
| 5 | measured temperature (int32, 0.1 °C) |
| 7 | measured humidity (not reported by the Wivar) |
| 8 | devices (`cDevice`) |
| 9 | setpoints per temp mode: `{ mode(1), value(2) }` |
| 11 / 13 | open-window detection enabled / detected |
| 16 | heating timer end (unix seconds), `temperatureBoosterFunctionEndDate` |

`cDevice`: id (1), batteryState (6, always 0 on the remote), signalStrength (7), sensors (8),
error codes (10), towel preheat end (39), fluidTemperature (60: the app's "Surface temperature" setting,
50 or 65 °C; shown only for remote firmware above U0.2.7) and selected duration (61). Error codes: 1 NoError,
2 BatteryLow, 3 SignalPoor, 4 SignalLost, 5 ValveDetached, 6 NoWarmWater, 7 room sensor,
8 medium sensor.

## Pairing

**AppPairing** (293 → 294) `{ appPairing(1): { uuid(1), nickName(2) }, remoteuuid(2): random 16 bytes }`.
The remote then flashes AUTO; a short press of MODE on the remote confirms (result 0), otherwise
result 10 after about 10 s. Pairing is tied to the app UUID.

## Writes

- **Setpoints**: SetRoomValue (205 → 206) `{ roomValue(1): { id(1), temperatures(6): [ { mode, value } ] } }`
  with all three setpoints (Home Awake, Home Asleep, Away); the bridge sends GetRooms first in the same
  connection to fill in the modes it doesn't change. The remote usually answers with a
  RoomsUpdateIndication carrying the new values instead of the confirm (captured 2026-10-14, see the replay
  test). It then doesn't answer reads for a few seconds. The app limits setpoints per mode: Home Awake
  15.0–28.0, Away 10.0–25.0, Antifreeze fixed 7.0; it offers no Home Asleep setpoint for the Wivar.
- **Heating timer**: SetDeviceFunctionMode (334 → 335)
  `{ mode(1): 1 = TOWEL_PREHEAT, state(2): 1 on / 0 off, deviceId(3), duration(4): minutes }`.
  The app offers 60/120/240 min, but any duration works (5 min confirmed). The end time then appears
  in room field 16.
  A SetRunState written after the timer ends it on the remote: field 16 goes back to 0, although the heater went on
  heating until the timer was stopped (seen 2026-10-25, also when the run mode was unchanged). The bridge therefore
  writes the run mode before the timer.

## Run mode

SetRunState (243 → 244) `{ runMode(1): 1 Automatic / 2 Manual / 3 Antifreeze, userLocation(2): 1 Home / 2 Away }`.
The Wivar supports Automatic, Manual and Antifreeze (the app's power button toggles Antifreeze). The app passes
the location matching the current temperature mode. Tested 2026-10-14: `{ Manual, Away }` gives Manual / Home
Awake: the Wivar's Manual is always Home. The remote answers with a RoomsUpdateIndication and a
RunStateUpdateIndication (242) carrying the new run state, then the confirm (244). In Manual, followingSchedule
(cRunstate 3) is 4; in Automatic 1. The run state (`cRunstate`, GetRunState 240 → 241) also carries
todaysDayplanId (5) and todaysWeekplanId (6).

## Schedule

- GetScheduleIds (177 → 178): `{ repeated dPlanIds(1), repeated wPlanIds(2) }`
- GetDayPlan (161 → 162) `{ dayplanId(1) }` → `{ dPlan(1): cDayplan { id(1), name(2), repeated dpItem(3) { startTime(1)
  minutes after midnight, tempMode(2) }, repeated functionDpItem(20) { startTime(1), mode(2) eFunctionMode,
  durationMin(3), roomId(4), deviceId(5) } } }`
- GetWeekPlan (163 → 164) `{ weekplanId(1) }` → `{ wPlan(1): cWeekplan { id(1), name(2), mondayId(3) .. sundayId(9) } }`

Writing (in `wivar_session`, not exposed to ESPHome; tested live 2026-10-14 by changing a day plan and writing the
original back, both verified by read-back): SetDayplan (173 → 174) `{ dPlan(1) }` and SetWeekplan (175 → 176)
`{ wPlan(1) }`, same layout as the Get confirms. Before the day plan confirm the remote sent rooms updates (196) and
a run-state update (242); no ScheduleUpdateIndication (160) `{ dPlan(1), wPlan(2) }` was seen. The app
locks plans (Lock/ReleaseLock Dayplan 150-153, Weekplan 154-157) only on devices with concurrent multi-user
support, which is off for the Wivar, so it just sends the Set request.

AddDayPlan (169 → 170) / AddWeekPlan (171 → 172) carry the complete plan (id 255, as in the app); the confirm
returns it with the id the remote assigned. The app's extra SetDayplan after an add is a workaround for firmware
U0.2.7 only. RemoveDayPlan (165 → 166) / RemoveWeekPlan (167 → 168) `{ id(1) }`; the app refuses to delete a day
plan a week plan uses. The bridge validates against the app's limits and runs one change at a time. It keeps the
change as a target rather than a list of requests: each write session first reads the whole schedule, plans the
requests from that read, and the next session's read either shows the target reached or continues from there. A
request without a confirm (timeout, dropped link) ends the round; nothing that depends on it is sent. The requests
are ordered so that stopping after any of them leaves every day on its old or its new content and no week plan
pointing at a missing day plan: unused plans the bridge made removed, adds, rewrites, the week plan, rewrites of
plans the week write moved days away from, removals. An unused day plan with a name the bridge gives (or none) is
taken as the bridge's own (e.g. an add whose confirm got lost) and reused or removed; other unused plans are left
alone.

The app's Wivar profile: at most 3 week plans, 8 day plans, 13 items per day plan, 5-minute steps. Its default day
plan is Away from 00:00, Home Awake from 07:00, towel preheat 90 min at 07:00.

## Clock

The remote runs the schedule on its own clock. On every connect the app sends SetTime (262 → 263)
`{ utcTime(1), daylightSaving(2): 2 Winter / 3 Summer, repeated switchTime(3) { localSwitchTime(1), daylightSaving(2) } }`
with the daylight-saving switches of the next 5 years; localSwitchTime is the local wall-clock time after the switch,
in seconds as if it were UTC. GetTime (260 → 261) `{ utcTime(1), localTime(2), daylightSaving(3), repeated
switchTime(4) }` reads it back, but the tested Wivar (firmware 0x100411) never answers it; the bridge then sets the
clock once a day without checking it.

## How the app talks to the remote

- On connect it reads the whole model once: site info, run state, rooms, paired apps, schedules.
- It then keeps the BLE connection open and has no polling timer. Changes arrive as indications pushed by the
  remote; a RoomsUpdateIndication replaces the app's room list.
- After its own writes it doesn't read back: it updates its local model and takes the remote's
  RoomsUpdateIndication as the answer.
- This is why the remote accepts only one connection and the bridge can't connect while the app is open.

