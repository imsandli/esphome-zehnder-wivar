#!/usr/bin/env python3
"""Full functional test of a running Wivar bridge over the ESPHome API. WRITES TO THE HEATER.

    .venv/bin/python tests/device_test.py HOST API_KEY [--web-user USER --web-password PW] [--no-heat]

Changes and restores, one after the other: Home and Away setpoint, Thermostat, Mode, a 15 min heating timer
(stopped again, also once together with a mode write; skip with --no-heat) and the schedule of the day after
tomorrow (written with the set_schedule action, restored with the "Schedule edit" text field). Also checks the
sensors, the power meter while heating (Shelly) and, with --web-user/--web-password, the web UI login.

The backup is taken after a refresh that read the room, the run state and the whole schedule from the remote (seen in
the bridge's log), not from the bridge's cached values; it is also saved to a local file. Refuses to start, and
stops before the mode test, while a heating timer runs (a mode write ends it on the remote, so it can't be kept).
The schedule is only changed if its backup is complete: the "Schedule" sensor shows at most 255 characters, the
remote must still hold it right before, and writing the backup back must leave that sensor unchanged before the
test schedule is written (it may rename day plans after their days, as any schedule write from the bridge does).
The sensor text doesn't show plan names or unused plans, so those aren't verified.

At the end each changed value is written back and compared with a fresh read; a dropped API connection is
reconnected and the step tried once more. Whatever can't be restored or verified is listed with its original value
("RECOVERY NEEDED"), and the backup file is kept; otherwise it is deleted. Takes about 20 minutes, because the
bridge keeps its sessions with the remote at least 20 s apart. Pairing is not tested (needs a press on the remote).
Exit code 0 = all checks passed.
"""
import argparse
import asyncio
import datetime
import json
import math
import os
import sys
import tempfile
import time
import urllib.error
import urllib.request

from aioesphomeapi import APIClient, LogLevel

DAYS = ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"]
WATCH = {"BLE status", "Schedule write", "Mode", "Run mode", "Home setpoint", "Away setpoint", "Target temperature",
         "Heat timer remaining", "Heating", "BLE connections", "Schedule", "Thermostat"}
# Bridge log lines that mean a refresh didn't read everything, although the session ended with "OK".
READ_FAILURES = ("GetRunState failed", "No run state from the remote", "Schedule read incomplete",
                 "Schedule read failed")
# "Heat timer remaining" is a template sensor updated every 15 s, not when the remote is read: after a read it can
# still show the time from before it for that long.
TIMER_SENSOR_S = 16
T0 = time.monotonic()
states: dict[str, object] = {}
logs: list[str] = []
results: list[tuple[str, bool, str]] = []


def log(msg: str) -> None:
    print(f"[{time.monotonic() - T0:5.0f}s] {msg}", flush=True)


def check(label: str, ok: bool, detail: object = "") -> bool:
    results.append((label, bool(ok), str(detail)))
    log(f"{'PASS' if ok else 'FAIL'}  {label}  {detail}")
    return bool(ok)


def close(a, b) -> bool:
    return isinstance(a, float) and isinstance(b, float) and abs(a - b) < 0.05


def valid(v) -> bool:
    return v not in (None, "") and not (isinstance(v, float) and math.isnan(v))


def same(a, b) -> bool:
    return close(a, b) if isinstance(b, float) else a == b


async def wait_for(name: str, pred, timeout: float) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            if pred(states.get(name)):
                return True
        except TypeError:
            pass
        await asyncio.sleep(1)
    return False


async def idle(timeout: float = 180) -> None:
    """Wait until the bridge has finished its session (status back to OK or Written)."""
    await asyncio.sleep(3)
    await wait_for("BLE status", lambda s: s in ("OK", "Written"), timeout)


async def session(send, timeout: float = 240) -> bool:
    """Send a command and wait until the bridge has connected to the remote for it and finished.

    Entities show a commanded value at once, before it is written, so only a new connection proves the write.
    """
    n = states.get("BLE connections") or 0
    send()
    ok = await wait_for("BLE connections", lambda v: v > n, timeout)
    await idle()
    return ok


def schedule_done(v) -> bool:
    return v in ("OK", "Unchanged") or str(v).startswith(("Failed", "Refused"))


def check_web(host: str, user: str, password: str) -> None:
    def status(u: str | None, p: str | None) -> int:
        opener = urllib.request.build_opener()
        if u is not None:
            mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
            mgr.add_password(None, f"http://{host}/", u, p)
            opener = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(mgr))
        try:
            return opener.open(f"http://{host}/", timeout=10).status
        except urllib.error.HTTPError as e:
            return e.code

    check("web UI refuses no login", status(None, None) == 401)
    check("web UI refuses wrong password", status(user, password + "x") == 401)
    check("web UI accepts digest login", status(user, password) == 200)


async def run(args) -> None:
    client = APIClient(args.host, 6053, None, noise_psk=args.api_key)
    await client.connect(login=True)
    entities, services = await client.list_entities_services()
    by_name = {e.name: e for e in entities}
    names = {e.key: e.name for e in entities}

    def on_state(s):
        n = names.get(s.key, str(s.key))
        v = getattr(s, "target_temperature", None) if n == "Thermostat" else getattr(s, "state", None)
        if n in WATCH and states.get(n) != v:
            log(f"      {n} = {v!r}")
        states[n] = v

    def on_log(m):
        logs.append(m.message.decode(errors="replace"))

    def subscribe() -> None:
        client.subscribe_states(on_state)
        client.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_INFO)

    async def reconnect() -> bool:
        for attempt in range(1, 4):
            try:
                await client.disconnect(force=True)
            except Exception:  # noqa: BLE001 - already gone
                pass
            try:
                await client.connect(login=True)
                subscribe()
                await asyncio.sleep(5)
                log("reconnected to the API")
                return True
            except Exception as e:  # noqa: BLE001
                log(f"reconnect {attempt}/3 failed: {e!r}")
                await asyncio.sleep(10)
        return False

    def key(n: str) -> int:
        return by_name[n].key

    async def read_back() -> bool:
        """Poll the remote, so the following checks see its values and not the commanded ones."""
        return await session(lambda: client.button_command(key("Refresh now")))

    async def fresh_read() -> bool:
        """Refresh, and check in the bridge's log that this session read the room, the run state and the schedule.

        A session that ended "OK" doesn't prove that: the run state and schedule reads can fail without failing it.
        On success, also waits until "Heat timer remaining" shows what this read reported.
        """
        for _ in range(2):  # a poll that was already starting may answer the first one, without the schedule
            await idle()
            start = len(logs)
            ok = await read_back() and states.get("BLE status") == "OK"
            lines = [m for m in logs[start:] if "[zehnder_wivar" in m]
            room = any("Room '" in m for m in lines)
            schedule = any("Schedule read:" in m for m in lines)
            failed = [m for m in lines if any(f in m for f in READ_FAILURES)]
            if ok and room and schedule and not failed:
                await asyncio.sleep(TIMER_SENSOR_S)
                return True
            log(f"refresh incomplete: session {ok}, room read {room}, schedule read {schedule}, failures {failed}")
        return False

    subscribe()
    await asyncio.sleep(5)
    set_schedule = next(s for s in services if s.name == "set_schedule")

    # The bridge's entities may be up to a poll old: a timer, setpoint or schedule changed on the remote since then
    # would be missed, and later overwritten with the stale backup.
    try:
        fresh, detail = await fresh_read(), ""
    except Exception as e:  # noqa: BLE001  (e.g. the API dropped; nothing has been written yet)
        fresh, detail = False, repr(e)
    if not check("fresh read before the backup", fresh, detail):
        try:
            await client.disconnect()
        except Exception:  # noqa: BLE001
            pass
        return
    orig = {n: states.get(n) for n in ("Home setpoint", "Away setpoint", "Mode", "Schedule", "Thermostat",
                                        "Heat timer remaining")}
    log(f"original: {orig}")
    if not check("initial state complete", all(valid(v) for v in orig.values()), orig):
        await client.disconnect()
        return
    # A running timer can't be restored: the mode test ends it on the remote, and a restart would only approximate it.
    if not check("no heating timer running", orig["Heat timer remaining"] == 0,
                 f"{orig['Heat timer remaining']} min left; run the test once it has ended"):
        await client.disconnect()
        return

    # Sensors (the Shelly's only where present).
    ranges = {"Temperature": (0, 40), "WiFi signal": (-100, -10), "Heater voltage": (200, 260),
              "Mains frequency": (45, 65), "Shelly temperature": (0, 90)}
    for n, (lo, hi) in ranges.items():
        if n in by_name:
            v = states.get(n)
            check(f"{n} plausible", valid(v) and lo <= v <= hi, v)
    for n in ("Target temperature", "Run mode", "Room", "Remote signal", "Remote firmware", "Schedule today",
              "Next switch", "Surface temperature setting", "BLE connected time", "Heater power", "Heater current",
              "Heater energy"):
        if n in by_name:
            check(f"{n} present", valid(states.get(n)), states.get(n))
    check("remote firmware tested", states.get("Remote firmware untested") is False,
          states.get("Remote firmware"))
    check("remote error none", states.get("Remote error") in ("None", None), states.get("Remote error"))
    if args.web_user is not None:
        check_web(args.host, args.web_user, args.web_password or "")

    # The user's settings: kept locally (not in the repository) until the restore is verified.
    fd, backup_path = tempfile.mkstemp(prefix="wivar-device-test-backup-", suffix=".json")
    with os.fdopen(fd, "w") as f:
        json.dump(orig, f, indent=1)
    log(f"backup saved to {backup_path}")

    schedule_written = False  # set before any schedule write is sent, the backup's round-trip included
    timer_started = False  # set before any "Heat 15 min" is sent; cleanup stops only a timer the test may have started
    mode_written = False  # set before any mode is sent; a mode write ends a running timer, so cleanup sends one only then
    try:
        # Refresh now
        await idle()
        n0 = states.get("BLE connections") or 0
        ok = await read_back()
        check("Refresh now polls", ok and states.get("BLE status") == "OK",
              f"connections {n0} -> {states.get('BLE connections')}")

        # Home and Away setpoint: each change is written, then read back from the remote.
        for n, hi in (("Home setpoint", 28.0), ("Away setpoint", 25.0)):
            o = orig[n]
            new = o + 0.5 if o + 0.5 <= hi else o - 0.5
            for target, label in ((new, f"{n} {o} -> {new}"), (o, f"{n} back to {o}")):
                wrote = await session(lambda t=target: client.number_command(key(n), t))
                read = await read_back()
                check(label, wrote and read and close(states.get(n), target),
                      f"{states.get(n)} (write session {wrote}, read back {read})")

        # Thermostat: changes the setpoint of the active mode (Home or Away, from the run mode).
        o = orig["Thermostat"]
        new = o + 0.5 if o + 0.5 <= 25.0 else o - 0.5
        active = "Home setpoint" if "Home" in str(states.get("Run mode")) else "Away setpoint"
        for target, label in ((new, f"Thermostat {o} -> {new}"), (o, f"Thermostat back to {o}")):
            wrote = await session(lambda t=target: client.climate_command(key("Thermostat"), target_temperature=t))
            read = await read_back()
            check(label, wrote and read and close(states.get("Target temperature"), target)
                  and close(states.get(active), target),
                  f"target {states.get('Target temperature')}, {active} {states.get(active)}, "
                  f"run mode {states.get('Run mode')}")

        # Mode. A timer started on the remote meanwhile would end with the mode write: check the last read (the
        # thermostat's), once the timer sensor shows it.
        await asyncio.sleep(TIMER_SENSOR_S)
        if not check("still no heating timer before the mode test", read and states.get("Heat timer remaining") == 0,
                     f"read back {read}, {states.get('Heat timer remaining')} min left; stopping"):
            return
        mode_written = True
        o = orig["Mode"]
        new = "Manual" if o != "Manual" else "Automatic"
        for target, label in ((new, f"Mode {o} -> {new}"), (o, f"Mode back to {o}")):
            wrote = await session(lambda t=target: client.select_command(key("Mode"), t))
            read = await read_back()
            check(label, wrote and read and states.get("Mode") == target
                  and str(states.get("Run mode", "")).startswith(target),
                  f"mode {states.get('Mode')}, run mode {states.get('Run mode')}")

        # Heating timer (read back by the bridge itself 10 s after the write)
        if not args.no_heat:
            timer_started = True
            client.button_command(key("Heat 15 min"))
            ok = await wait_for("Heat timer remaining", lambda v: v > 0, 150)
            check("Heat 15 min starts the timer", ok, f"{states.get('Heat timer remaining')} min")
            if "Heating" in by_name:
                ok = await wait_for("Heating", lambda v: v is True, 120)
                check("heater draws power", ok, f"{states.get('Heater power')} W")
            # Until the remote has been read, the remaining time is the bridge's own countdown. After a read it
            # comes from the end time the remote reports, compared with the bridge's clock: a remote clock that is
            # off (e.g. not yet set after a daylight-saving switch) shows up here.
            await idle()
            read = await read_back()
            remaining = states.get("Heat timer remaining")
            check("timer reported by the remote", read and isinstance(remaining, float) and 10 <= remaining <= 15,
                  f"{remaining} min")
            client.button_command(key("Heat timer stop"))
            ok = await wait_for("Heat timer remaining", lambda v: v == 0, 150)
            await idle()
            check("Heat timer stop", ok, f"{states.get('Heat timer remaining')} min")
            if "Heating" in by_name:
                ok = await wait_for("Heating", lambda v: v is False, 120)
                check("heater stops drawing power", ok, f"{states.get('Heater power')} W")

            # Timer and mode in one session: a run-mode write after the timer would end it on the remote.
            def timer_and_mode():
                client.button_command(key("Heat 15 min"))
                client.select_command(key("Mode"), orig["Mode"])

            await session(timer_and_mode)
            await asyncio.sleep(30)  # the bridge's own read-back
            await idle()
            read = await read_back()
            remaining = states.get("Heat timer remaining")
            check("timer survives a mode write in the same session",
                  read and isinstance(remaining, float) and 10 <= remaining <= 15, f"{remaining} min")
            await session(lambda: client.button_command(key("Heat timer stop")))
            await read_back()
            check("Heat timer stop (after timer and mode)", states.get("Heat timer remaining") == 0,
                  f"{states.get('Heat timer remaining')} min")

        # Schedule: changed only with a backup that restores it. The sensor cuts text over 255 characters to 252 plus
        # "...", and the text must parse back to what the remote holds: writing it must leave the sensor as it was
        # ("OK" instead of "Unchanged" when it renamed app-named day plans after their days).
        backup_ok = check("schedule backup not truncated", not (len(orig["Schedule"]) >= 255
                                                                 and orig["Schedule"].endswith("...")),
                          f"{len(orig['Schedule'])} characters")
        if backup_ok:
            # Changed on the remote since the backup: writing the backup would overwrite that.
            fresh = await fresh_read()
            backup_ok = check("schedule still as backed up", fresh and states.get("Schedule") == orig["Schedule"],
                              f"fresh read {fresh} | {states.get('Schedule')}")
        if backup_ok:
            states["Schedule write"] = None
            schedule_written = True
            await client.execute_service(set_schedule, {"schedule": orig["Schedule"]})
            ok = await wait_for("Schedule write", schedule_done, 300)
            await idle()
            backup_ok = check("schedule backup round-trips", ok and states.get("Schedule write") in ("OK", "Unchanged")
                              and states.get("Schedule") == orig["Schedule"],
                              f"{states.get('Schedule write')} | {states.get('Schedule')}")
        if backup_ok:
            # The day after tomorrow, so the mode right now can't change.
            day = DAYS[(datetime.date.today().weekday() + 2) % 7]
            test = f"{day}: 00:00 Away, 03:00 Home, 03:30 Away"
            states["Schedule write"] = None
            await client.execute_service(set_schedule, {"schedule": test})
            ok = await wait_for("Schedule write", schedule_done, 300)
            await idle()
            sched = str(states.get("Schedule"))
            check(f"set_schedule action ({test})", ok and states.get("Schedule write") == "OK"
                  and sched != orig["Schedule"] and "03:00 Home" in sched, f"{states.get('Schedule write')} | {sched}")
        elif schedule_written:
            log("schedule test skipped: writing the backup back didn't reproduce it; restoring it below")
        else:
            log("schedule test skipped, nothing written to the schedule")
    except Exception as e:  # noqa: BLE001 - e.g. the API connection dropped; restore what can be restored
        check("test ran to the end", False, repr(e))
    finally:
        log("restoring the original state")
        restore_failed = False

        async def step(label: str, fn) -> None:
            """Run one restore step; after an error (API dropped) reconnect and try once more, then go on."""
            nonlocal restore_failed
            for attempt in (1, 2):
                try:
                    await fn()
                    return
                except Exception as e:  # noqa: BLE001
                    log(f"restore step '{label}' failed: {e!r}")
                    if attempt == 2 or not await reconnect():
                        break
            restore_failed = True
            check(f"restore step: {label}", False, "not completed")

        fresh = False

        async def read_remote() -> None:
            nonlocal fresh
            fresh = False  # a failed read must not leave an earlier read's result standing
            await idle(240)
            fresh = await fresh_read()  # compare against the remote's values, not the last commanded ones

        async def restore_number(n: str) -> None:
            if not fresh or not close(states.get(n), orig[n]):
                await session(lambda: client.number_command(key(n), orig[n]))

        async def restore_mode() -> None:
            if mode_written and (not fresh or states.get("Mode") != orig["Mode"]):
                await session(lambda: client.select_command(key("Mode"), orig["Mode"]))

        async def stop_timer() -> None:
            # Only a timer this test may have started: one started on the remote meanwhile is left running.
            if timer_started and (not fresh or (states.get("Heat timer remaining") or 0) > 0):
                await session(lambda: client.button_command(key("Heat timer stop")))

        async def restore_schedule() -> None:
            # Only after a schedule write, with the verified backup; an unverified one could be truncated.
            if schedule_written and (not fresh or states.get("Schedule") != orig["Schedule"]):
                states["Schedule write"] = None
                client.text_command(key("Schedule edit"), orig["Schedule"])
                ok = await wait_for("Schedule write", schedule_done, 300)
                await idle()
                check("schedule restored (Schedule edit)", ok and states.get("Schedule") == orig["Schedule"],
                      f"{states.get('Schedule write')} | {states.get('Schedule')}")

        await step("read the remote", read_remote)
        await step("Home setpoint", lambda: restore_number("Home setpoint"))
        await step("Away setpoint", lambda: restore_number("Away setpoint"))
        await step("Mode", restore_mode)
        await step("heating timer", stop_timer)
        await step("schedule", restore_schedule)
        await step("read the remote", read_remote)
        final = {n: states.get(n) for n in orig}
        differ = {n: f"{final[n]!r}, original {orig[n]!r}" for n in orig if not same(final[n], orig[n])}
        check("final state read from the remote", fresh, "values below may be the bridge's cached ones")
        check("final state equals the original", not differ, differ or final)
        check("remote error none at the end", states.get("Remote error") in ("None", None), states.get("Remote error"))
        if differ or not fresh or restore_failed:
            what = differ if fresh else {n: f"original {orig[n]!r}" for n in orig}
            log(f"RECOVERY NEEDED: not restored{'' if fresh and not restore_failed else ' or not verified'}: "
                f"{what or 'a restore step failed'}. Backup kept in {backup_path}")
        else:
            os.remove(backup_path)
        try:
            await client.disconnect()
        except Exception:  # noqa: BLE001
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host")
    ap.add_argument("api_key")
    ap.add_argument("--web-user", help="also check the web UI's digest login")
    ap.add_argument("--web-password")
    ap.add_argument("--no-heat", action="store_true", help="skip the heating timer test")
    asyncio.run(run(ap.parse_args()))
    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    for label, _, detail in failed:
        print(f"FAIL  {label}  {detail}")
    return 1 if failed or not results else 0


if __name__ == "__main__":
    sys.exit(main())
