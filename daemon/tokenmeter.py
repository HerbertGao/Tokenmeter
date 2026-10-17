#!/usr/bin/env python3
"""Pi's local usage -> Tokenmeter BLE. --once requires only Python's stdlib."""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
from pathlib import Path
import sys

from .usage import UsageScanner, encode_snapshot, unavailable_snapshot

SERVICE_UUID = '544f4b45-4e4d-4554-4552-000000000001'
RX_UUID = '544f4b45-4e4d-4554-4552-000000000002'
ACK_UUID = '544f4b45-4e4d-4554-4552-000000000003'
LOG = logging.getLogger('tokenmeter')


def collect(scanner: UsageScanner) -> tuple[dict, bytes]:
    snapshot = scanner.snapshot()
    try:
        payload = encode_snapshot(snapshot)
    except ValueError as error:
        LOG.error('%s; sending unavailable, not truncated data', error)
        snapshot = unavailable_snapshot(issues=max(1, snapshot['issues']))
        payload = encode_snapshot(snapshot)
    return snapshot, payload


async def write_snapshot(client, payload: bytes, acknowledgements: asyncio.Queue) -> None:
    while not acknowledgements.empty():
        acknowledgements.get_nowait()
    size = max(1, min(180, client.mtu_size - 3))
    for offset in range(0, len(payload), size):
        await asyncio.wait_for(client.write_gatt_char(RX_UUID, payload[offset:offset + size], response=True), 10)
    acknowledgement = await asyncio.wait_for(acknowledgements.get(), 10)
    if acknowledgement != b'ok':
        raise RuntimeError('device rejected snapshot')


async def run(scanner: UsageScanner, interval: float, address: str | None) -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as error:
        raise RuntimeError('Install daemon/requirements.txt into a virtual environment first') from error
    retry = 5
    while True:
        try:
            target = address
            if not target:
                LOG.info('Looking for Tokenmeter service')
                target = await BleakScanner.find_device_by_filter(
                    lambda _, advert: SERVICE_UUID in (u.lower() for u in advert.service_uuids), timeout=15)
                if target is None:
                    raise RuntimeError('Tokenmeter not found (old Clawdmeter firmware uses a different service)')
            async with BleakClient(target, services=[SERVICE_UUID]) as client:
                retry = 5
                acknowledgements = asyncio.Queue(maxsize=1)

                def acknowledged(_, data):
                    if not acknowledgements.full():
                        acknowledgements.put_nowait(bytes(data))

                await client.start_notify(ACK_UUID, acknowledged)
                LOG.info('Connected to Tokenmeter')
                while client.is_connected:
                    snapshot, payload = await asyncio.to_thread(collect, scanner)
                    await write_snapshot(client, payload, acknowledgements)
                    LOG.info('Updated: %s, %d records, %d pairs, %d source issues', snapshot['status'],
                             snapshot['totals']['records'], len(snapshot['models']), snapshot['issues'])
                    await asyncio.sleep(interval)
        except asyncio.CancelledError:
            raise
        except Exception as error:
            LOG.warning('BLE update failed: %s; retry in %ds', error, retry)
            await asyncio.sleep(retry)
            retry = min(60, retry * 2)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pi-dir', type=Path, action='append',
                        help='Pi agent directory containing sessions/; repeat for multiple roots')
    parser.add_argument('--once', action='store_true', help='print one usage-only snapshot without Bluetooth')
    parser.add_argument('--interval', type=float, default=60, help='refresh seconds (default: 60, maximum 120)')
    parser.add_argument('--address', help='optional BLE address/macOS peripheral UUID to select one device')
    args = parser.parse_args(argv)
    if not 1 <= args.interval <= 120:
        parser.error('--interval must be between 1 and 120 seconds')
    roots = args.pi_dir or [Path(os.environ.get('PI_AGENT_DIR') or Path.home() / '.pi' / 'agent')]
    scanner = UsageScanner(roots)
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s', stream=sys.stderr)
    if args.once:
        snapshot, _ = collect(scanner)
        print(json.dumps(snapshot, ensure_ascii=False, indent=2, allow_nan=False))
        return 2 if snapshot['status'] == 'unavailable' else 0
    try:
        asyncio.run(run(scanner, args.interval, args.address))
    except KeyboardInterrupt:
        return 130
    except RuntimeError as error:
        LOG.error('%s', error)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
