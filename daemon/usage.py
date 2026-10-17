"""Local Pi usage metadata; no credentials, content output, or persistent index."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, time, timedelta
from decimal import Decimal, InvalidOperation
import json
import math
import os
from pathlib import Path
import time as clock

MAX_FRAME_BYTES = 65536
MAX_MODELS = 256
MAX_ID_BYTES = 192
MAX_COUNT = 9007199254740991
MAX_COST = Decimal('1000000000000')
TOKEN_FIELDS = ('input', 'output', 'cacheRead', 'cacheWrite')


def day_window(now: float) -> tuple[str, float, float, int]:
    local = datetime.fromtimestamp(now)
    start = datetime.combine(local.date(), time()).timestamp()
    end = datetime.combine(local.date() + timedelta(days=1), time()).timestamp()
    offset = datetime.fromtimestamp(now).astimezone().utcoffset()
    assert offset is not None
    return local.date().isoformat(), start, end, int(offset.total_seconds() // 60)


def empty_totals(cost: float | None = 0.0) -> dict:
    return dict(cost=cost, tokens=0, input=0, output=0, cache_read=0,
                cache_write=0, records=0, unpriced=0)


def unavailable_snapshot(now: float | None = None, issues: int = 1) -> dict:
    now = clock.time() if now is None else now
    date, _, end, offset = day_window(now)
    return dict(v=1, date=date, epoch=int(now), day_end=int(end), utc_offset=offset,
                status='unavailable', issues=issues, totals=empty_totals(None), models=[])


def encode_snapshot(snapshot: dict) -> bytes:
    """Validate device bounds; never silently omit identities or rows."""
    rows = snapshot['models']
    if len(rows) > MAX_MODELS:
        raise ValueError('too many provider/model pairs')
    for row in rows:
        for key in ('provider', 'model'):
            if len(row[key].encode('utf-8')) > MAX_ID_BYTES:
                raise ValueError('provider/model identity exceeds device limit')
    for values in [snapshot['totals'], *rows]:
        cost = values['cost']
        if cost is not None and (not math.isfinite(cost) or not 0 <= cost <= MAX_COST):
            raise ValueError('cost exceeds device limit')
        for key, value in values.items():
            if key not in ('provider', 'model', 'cost'):
                if type(value) is not int or not 0 <= value <= MAX_COUNT:
                    raise ValueError('count exceeds device limit')
    raw = json.dumps(snapshot, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode('utf-8')
    if len(raw) > MAX_FRAME_BYTES:
        raise ValueError('snapshot exceeds device frame limit')
    return raw + b'\n'


def _number(value) -> Decimal | None:
    if isinstance(value, bool) or not isinstance(value, (int, float, Decimal)):
        return None
    try:
        number = Decimal(str(value))
        return number if number.is_finite() and number >= 0 else None
    except InvalidOperation:
        return None


def _price(value) -> Decimal | None:
    number = _number(value)
    return number if number is not None and number <= MAX_COST else None


def _count(value) -> int | None:
    number = _number(value)
    return int(number) if number is not None and number <= MAX_COUNT and number == number.to_integral_value() else None


def _timestamp(entry: dict, message: dict) -> float | None:
    for value in (entry.get('timestamp'), message.get('timestamp')):
        try:
            if isinstance(value, str):
                result = datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp()
            elif isinstance(value, (int, float, Decimal)) and not isinstance(value, bool) and math.isfinite(value):
                result = float(value / 1000 if value > 100000000000 else value)
            else:
                continue
            if math.isfinite(result):
                return result
        except (ValueError, OverflowError, OSError):
            pass
    return None


def _identity(value) -> str | None:
    return value if isinstance(value, str) and value.strip() and not any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in value) else None


@dataclass(frozen=True)
class Record:
    key: tuple
    timestamp: float
    provider: str
    model: str
    splits: tuple[int, ...]
    tokens: int
    cost: Decimal | None


def _record(entry: dict, path: Path, line: int, start: float, end: float) -> tuple[Record | None, int]:
    kind = entry.get('type')
    message = entry.get('message')
    message = message if isinstance(message, dict) else {}
    if kind == 'message' and message.get('role') in ('assistant', 'toolResult'):
        usage = message.get('usage')
        attribution = message
        kind = message['role']
    elif kind in ('usage', 'compaction', 'branch_summary'):
        usage = entry.get('usage')
        attribution = entry
    else:
        return None, 0
    if usage is None:
        return None, 0
    if not isinstance(usage, dict):
        return None, 1
    timestamp = _timestamp(entry, message)
    if timestamp is None:
        return None, 1
    if not start <= timestamp < end:
        return None, 0
    provider = _identity(attribution.get('provider')) or 'unknown'
    model = _identity(attribution.get('responseModel')) or _identity(attribution.get('model')) or 'unknown'
    parts = tuple(_count(usage.get(k)) for k in TOKEN_FIELDS)
    splits = tuple(value or 0 for value in parts)
    issues = int(any(value is None for value in parts))
    # Pi's native cost breakdown sums these four parts; reasoning is already output.
    tokens = sum(splits)
    if issues:
        fallback = _count(usage.get('totalTokens'))
        if fallback is not None:
            tokens = max(tokens, fallback)
    raw_cost = usage.get('cost')
    cost = _price(raw_cost.get('total')) if isinstance(raw_cost, dict) else None
    if provider == 'qoder' and (cost is None or cost == 0):
        equivalent = _price(usage.get('usd_equivalent'))
        if equivalent is not None:
            cost = equivalent
    # An all-zero failed/default usage is not a billable usage record.
    if tokens == 0 and cost == 0:
        return None, issues
    event_id = entry.get('id')
    if isinstance(event_id, str) and event_id:
        key = (kind, event_id, int(timestamp * 1000000), provider, model)
    else:
        # shortcut: no stable event ID cannot be deduplicated across copies; do not guess from content.
        key = ('file', str(path), line)
    return Record(key, timestamp, provider, model, splits, tokens, cost), issues


@dataclass
class CachedFile:
    signature: tuple[int, int, int, int, int]
    records: list[Record]
    issues: int


class UsageScanner:
    def __init__(self, agent_dirs: list[Path]):
        self.roots = sorted({p.expanduser().resolve() / 'sessions' for p in agent_dirs})
        self.cache: dict[Path, CachedFile] = {}
        self.window: tuple[float, float] | None = None

    def _read(self, path: Path, signature: tuple, start: float, end: float) -> CachedFile:
        records, issues = [], 0
        with path.open('rb') as stream:
            for number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    # Parse the JSONL container; retain ONLY the Record allowlist, never content.
                    entry = json.loads(line, parse_float=Decimal)
                except (ValueError, UnicodeError, RecursionError, InvalidOperation, OverflowError):
                    # A writer may not have finished its trailing line yet.
                    issues += int(line.endswith(b'\n'))
                    continue
                if not isinstance(entry, dict):
                    issues += 1
                    continue
                record, failures = _record(entry, path, number, start, end)
                issues += failures
                if record is not None:
                    records.append(record)
        return CachedFile(signature, records, issues)

    def snapshot(self, now: float | None = None) -> dict:
        now = clock.time() if now is None else now
        date, start, end, offset = day_window(now)
        if self.window != (start, end):
            self.cache.clear()
            self.window = (start, end)
        paths, issues, readable_roots = set(), 0, 0
        for root in self.roots:
            if not root.is_dir():
                issues += 1
                continue
            errors = []
            root_paths = set()
            for directory, _, names in os.walk(root, onerror=errors.append, followlinks=False):
                for name in names:
                    if name.endswith('.jsonl'):
                        path = (Path(directory) / name).resolve()
                        if path.suffix == '.jsonl':
                            root_paths.add(path)
            issues += len(errors)
            if not errors:
                readable_roots += 1
            paths.update(root_paths)
        unique: dict[tuple, Record] = {}
        readable_files = 0
        for path in sorted(paths):
            try:
                stat = path.stat()
                signature = (stat.st_dev, stat.st_ino, stat.st_mtime_ns, stat.st_ctime_ns, stat.st_size)
                cached = self.cache.get(path)
                if cached is None or cached.signature != signature:
                    cached = self._read(path, signature, start, end)
                    self.cache[path] = cached
                readable_files += 1
                issues += cached.issues
                for record in cached.records:
                    if record.timestamp > now:
                        continue
                    previous = unique.get(record.key)
                    if previous is None:
                        unique[record.key] = record
                    elif previous != record:
                        issues += 1
                        # Prefer a priced/more complete copy, but disclose any inconsistency.
                        rank = lambda r: (r.cost is not None, r.cost or Decimal(0), r.tokens)
                        unique[record.key] = max((previous, record), key=rank)
            except (OSError, ValueError, OverflowError):
                issues += 1
                self.cache.pop(path, None)
        self.cache = {p: c for p, c in self.cache.items() if p in paths}
        if readable_roots == 0 and readable_files == 0 or paths and readable_files == 0:
            return unavailable_snapshot(now, max(1, issues))
        groups: dict[tuple[str, str], dict] = {}
        totals = empty_totals()
        total_cost = Decimal(0)
        priced = 0
        for record in unique.values():
            pair = (record.provider, record.model)
            group = groups.setdefault(pair, dict(provider=pair[0], model=pair[1], cost=Decimal(0),
                                                   tokens=0, records=0, unpriced=0))
            group['tokens'] += record.tokens
            group['records'] += 1
            totals['tokens'] += record.tokens
            totals['records'] += 1
            for field, value in zip(('input', 'output', 'cache_read', 'cache_write'), record.splits):
                totals[field] += value
            if record.cost is None:
                group['unpriced'] += 1
                totals['unpriced'] += 1
            else:
                group['cost'] += record.cost
                total_cost += record.cost
                priced += 1
        rows = list(groups.values())
        for row in rows:
            if row['unpriced'] == row['records']:
                row['cost'] = None
        rows.sort(key=lambda r: (r['cost'] is None, -(r['cost'] or Decimal(0)), r['provider'], r['model']))
        for row in rows:
            if row['cost'] is not None:
                row['cost'] = float(row['cost'])
        totals['cost'] = float(total_cost) if priced or not unique and not issues else None
        return dict(v=1, date=date, epoch=int(now), day_end=int(end), utc_offset=offset,
                    status='partial' if issues else 'ok', issues=issues, totals=totals, models=rows)
