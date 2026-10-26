#!/usr/bin/env python3
"""Read-only smoke test of a running Wivar bridge over the ESPHome API.

    .venv/bin/python tests/smoke.py HOST API_KEY [--wait 60]

Connects, waits for state updates and checks that the bridge reaches the remote: BLE status OK
(or a recent reading), a plausible temperature and no remote errors. Sends nothing to the heater.
Exit code 0 = healthy.
"""
import argparse
import asyncio
import math
import sys

from aioesphomeapi import APIClient


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host")
    ap.add_argument("api_key")
    ap.add_argument("--wait", type=int, default=60, help="seconds to collect states (default 60)")
    args = ap.parse_args()

    client = APIClient(args.host, 6053, None, noise_psk=args.api_key)
    await client.connect(login=True)
    entities, _ = await client.list_entities_services()
    names = {e.key: e.name for e in entities}
    states: dict[str, object] = {}
    client.subscribe_states(lambda s: states.__setitem__(names.get(s.key, str(s.key)), getattr(s, "state", None)))
    await asyncio.sleep(args.wait)
    await client.disconnect()

    checks = []
    temp = states.get("Temperature")
    checks.append(("temperature plausible", isinstance(temp, float) and not math.isnan(temp) and 0 < temp < 40, temp))
    checks.append(("remote error none", states.get("Remote error") in ("None", None), states.get("Remote error")))
    checks.append(("battery not low", states.get("Remote battery low") is not True, states.get("Remote battery low")))
    checks.append(("not overheated", states.get("Heater overheated") is not True, states.get("Heater overheated")))
    status = states.get("BLE status")
    checks.append(("BLE status", status in ("OK", "Written", "Connecting", "Reading", "Waiting"), status))
    for name in ("Run mode", "Target temperature", "Heat timer remaining"):
        print(f"  {name:22} {states.get(name)}")
    ok = True
    for label, passed, value in checks:
        print(f"{'PASS' if passed else 'FAIL'}  {label:22} {value}")
        ok &= bool(passed)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
