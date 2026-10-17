import asyncio
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from daemon.usage import UsageScanner, day_window, encode_snapshot, MAX_COUNT, MAX_MODELS, MAX_COST, MAX_FRAME_BYTES
from daemon.tokenmeter import RX_UUID, collect, write_snapshot

NOW = datetime(2026, 10, 16, 12, tzinfo=timezone.utc).timestamp()


def event(event_id='entry', timestamp=NOW - 10, provider='provider', model='model') -> dict:
    return dict(type='message', id=event_id,
                timestamp=datetime.fromtimestamp(timestamp, timezone.utc).isoformat(),
                message=dict(role='assistant', provider=provider, model=model,
                             timestamp=int(timestamp * 1000), stopReason='stop',
                             content=[dict(type='text', text='PRIVATE_DO_NOT_EXPORT')],
                             usage=dict(input=10, output=2, cacheRead=3, cacheWrite=1,
                                        totalTokens=16, cost=dict(total=.25))))


class UsageTests(unittest.TestCase):
    def setUp(self):
        self.old_tz = os.environ.get('TZ')
        os.environ['TZ'] = 'UTC'
        time.tzset()
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / 'pi'
        self.sessions = self.root / 'sessions'
        self.sessions.mkdir(parents=True)
        self.scanner = UsageScanner([self.root])

    def tearDown(self):
        self.tmp.cleanup()
        if self.old_tz is None:
            os.environ.pop('TZ', None)
        else:
            os.environ['TZ'] = self.old_tz
        time.tzset()

    def save(self, name, *entries):
        path = self.sessions / name
        path.write_text(''.join(json.dumps(e) + '\n' for e in entries))
        return path

    def test_forks_deduplicate_but_models_providers_and_distinct_events_do_not(self):
        original = event()
        self.save('original.jsonl', original)
        self.save('fork.jsonl', original, event('next', provider='other'),
                  event('entry', timestamp=NOW - 5))
        snapshot = self.scanner.snapshot(NOW)
        self.assertEqual(snapshot['totals']['records'], 3)
        self.assertEqual(snapshot['totals']['tokens'], 48)
        self.assertEqual(snapshot['totals']['cost'], .75)
        self.assertEqual(len(snapshot['models']), 2)
        self.assertNotIn('PRIVATE_DO_NOT_EXPORT', json.dumps(snapshot))
        self.assertNotIn('PRIVATE_DO_NOT_EXPORT', repr(self.scanner.cache))
        self.assertEqual(snapshot['status'], 'ok')

    def test_errors_with_usage_are_counted_and_zero_failure_is_not(self):
        a, b, c = event('a'), event('b'), event('c')
        a['message']['stopReason'] = 'error'
        b['message']['stopReason'] = 'aborted'
        c['message']['stopReason'] = 'error'
        c['message']['usage'] = dict(input=0, output=0, cacheRead=0, cacheWrite=0, totalTokens=0, cost=dict(total=0))
        self.save('a.jsonl', a, b, c)
        self.assertEqual(self.scanner.snapshot(NOW)['totals']['records'], 2)

    def test_native_four_parts_reasoning_response_model_and_qoder(self):
        a = event(provider='qoder')
        a['message']['responseModel'] = 'actual-model'
        a['message']['usage'].update(totalTokens=12, reasoning=2, cacheWrite1h=1, usd_equivalent=.21723296)
        a['message']['usage']['cost']['total'] = 0
        self.save('a.jsonl', a)
        snapshot = self.scanner.snapshot(NOW)
        self.assertEqual(snapshot['totals']['tokens'], 16)
        self.assertEqual(snapshot['totals']['cost'], .21723296)
        self.assertEqual(snapshot['models'][0]['model'], 'actual-model')
        a['message']['usage']['usd_equivalent'] = 0
        a['message']['responseModel'] = 'unknown'
        self.save('a.jsonl', a)
        snapshot = self.scanner.snapshot(NOW)
        self.assertEqual(snapshot['totals']['cost'], 0)
        self.assertEqual(snapshot['models'][0]['model'], 'unknown')

    def test_extra_usage_kinds_and_unknown_attribution(self):
        a = event('tool'); a['message']['role'] = 'toolResult'
        del a['message']['provider']; del a['message']['model']
        entries = [a]
        for kind in ('usage', 'compaction', 'branch_summary'):
            entries.append(dict(type=kind, id=kind, timestamp=a['timestamp'], usage=a['message']['usage']))
        entries[1].update(provider='explicit', model='explicit-model')
        self.save('a.jsonl', *entries)
        snapshot = self.scanner.snapshot(NOW)
        self.assertEqual(snapshot['totals']['records'], 4)
        self.assertEqual(snapshot['totals']['cost'], 1)
        unknown = next(r for r in snapshot['models'] if r['provider'] == 'unknown')
        self.assertEqual(unknown['model'], 'unknown')
        self.assertEqual(unknown['records'], 3)

    def test_missing_prices_are_not_free_and_known_zero_is_free(self):
        a = event('a'); a['message']['usage'].pop('cost')
        self.save('a.jsonl', a)
        snapshot = self.scanner.snapshot(NOW)
        self.assertIsNone(snapshot['totals']['cost'])
        self.assertEqual(snapshot['totals']['unpriced'], 1)
        b = event('b'); b['message']['usage']['cost']['total'] = 0
        self.save('b.jsonl', b)
        snapshot = self.scanner.snapshot(NOW)
        self.assertEqual(snapshot['totals']['cost'], 0)
        self.assertEqual(snapshot['models'][0]['unpriced'], 1)

    def test_missing_source_is_not_empty_day(self):
        empty = self.scanner.snapshot(NOW)
        self.assertEqual(empty['totals']['cost'], 0)
        self.assertEqual(empty['yesterday'], dict(date='2026-10-15', status='ok', issues=0, totals=empty['totals']))
        missing = UsageScanner([self.root / 'missing']).snapshot(NOW)
        self.assertEqual(missing['status'], 'unavailable')
        self.assertIsNone(missing['totals']['cost'])
        self.assertEqual(missing['yesterday'], dict(date='2026-10-15', status='unavailable',
                                                  issues=missing['issues'], totals=missing['totals']))
        partial = UsageScanner([self.root, self.root / 'missing']).snapshot(NOW)
        self.assertEqual(partial['status'], 'partial')
        self.assertEqual(partial['yesterday']['status'], 'partial')
        self.assertEqual(partial['yesterday']['issues'], partial['issues'])

    def test_missing_splits_fallback_and_malformed_complete_vs_unfinished_line(self):
        a = event(); a['message']['usage'] = dict(totalTokens=99, cost=dict(total=.5))
        path = self.save('a.jsonl', a)
        with path.open('a') as f: f.write('{"unfinished":')
        snapshot = self.scanner.snapshot(NOW)
        self.assertEqual(snapshot['totals']['tokens'], 99)
        self.assertEqual(snapshot['issues'], 1)  # missing splits, not the unfinished tail
        with path.open('a') as f: f.write('bad}\n')
        malformed = self.scanner.snapshot(NOW)
        self.assertEqual(malformed['issues'], 2)
        self.assertEqual(malformed['yesterday']['issues'], 2)
        self.assertEqual(malformed['yesterday']['status'], 'partial')
        self.assertIsNone(malformed['yesterday']['totals']['cost'])

    def test_cache_future_records_and_day_change(self):
        self.save('a.jsonl', event('today'), event('future', timestamp=NOW + 30),
                  event('yesterday', timestamp=NOW - 86400))
        self.assertEqual(self.scanner.snapshot(NOW)['totals']['records'], 1)
        with patch.object(self.scanner, '_read', side_effect=AssertionError('unchanged file was reread')):
            self.assertEqual(self.scanner.snapshot(NOW + 60)['totals']['records'], 2)
        self.assertEqual(self.scanner.snapshot(NOW + 86400)['totals']['records'], 0)

    def test_rewrite_and_overlapping_roots(self):
        self.save('a.jsonl', event('a'), event('b'))
        self.assertEqual(self.scanner.snapshot(NOW)['totals']['records'], 2)
        self.save('a.jsonl', event('new'))
        self.assertEqual(self.scanner.snapshot(NOW)['totals']['records'], 1)
        path = self.sessions / 'a.jsonl'
        old = path.stat()
        changed = event('new'); changed['message']['usage']['cost']['total'] = .35
        self.save('a.jsonl', changed)
        os.utime(path, ns=(old.st_atime_ns, old.st_mtime_ns))
        self.assertEqual(path.stat().st_size, old.st_size)
        self.assertEqual(self.scanner.snapshot(NOW)['totals']['cost'], .35)
        overlapping = UsageScanner([self.root, self.root / '..' / 'pi'])
        self.assertEqual(overlapping.snapshot(NOW)['totals']['records'], 1)

    def test_dst_days_and_boundary(self):
        os.environ['TZ'] = 'America/New_York'; time.tzset()
        for date, hours in [((2026, 3, 8), 23), ((2026, 11, 1), 25)]:
            noon = datetime(*date, 12).timestamp()
            _, start, end, _ = day_window(noon)
            self.assertEqual(end - start, hours * 3600)
        _, start, end, _ = day_window(NOW)
        self.save('a.jsonl', event('start', timestamp=start), event('before', timestamp=start - .001), event('end', timestamp=end))
        self.assertEqual(self.scanner.snapshot(NOW)['totals']['records'], 1)

    def test_two_days_boundaries_dedup_and_midnight_rollover(self):
        today = datetime(2026, 10, 16, tzinfo=timezone.utc).timestamp()
        records = [event('old', timestamp=today - 86400 - .001),
                   event('yesterday', timestamp=today - 86400, model='yesterday-model'),
                   event('today', timestamp=today, model='today-model'),
                   event('future', timestamp=today + 86400, model='future-model')]
        self.save('source.jsonl', *records)
        self.save('copy.jsonl', *records)
        with patch.object(self.scanner, '_read', wraps=self.scanner._read) as read:
            s = self.scanner.snapshot(NOW)
            self.scanner.snapshot(NOW)
            self.assertEqual(read.call_count, 2)
            self.assertTrue(all(len(cached.records) == 2 for cached in self.scanner.cache.values()))
            self.assertEqual(s['totals']['tokens'], 16)
            self.assertEqual(s['totals']['cost'], .25)
            self.assertEqual(s['totals']['records'], 1)
            self.assertEqual([row['model'] for row in s['models']], ['today-model'])
            self.assertEqual(s['yesterday']['totals'], s['totals'])
            self.assertEqual(set(s['yesterday']), {'date', 'status', 'issues', 'totals'})
            rolled = self.scanner.snapshot(today + 86400)
            self.assertEqual(read.call_count, 4)
            self.assertEqual(rolled['date'], '2026-10-17')
            self.assertEqual(rolled['totals']['records'], 1)
            self.assertEqual(rolled['yesterday']['date'], '2026-10-16')
            self.assertEqual(rolled['yesterday']['totals'], s['totals'])
            self.assertEqual([row['model'] for row in rolled['models']], ['future-model'])

    def test_yesterday_only_needs_no_model_or_identity_payload(self):
        provider = 'private-provider-' + 'p' * 200
        self.save('session.jsonl', *(event(str(i), timestamp=NOW - 86400, provider=provider, model=str(i) + 'm' * 200)
                                     for i in range(MAX_MODELS + 1)))
        s = self.scanner.snapshot(NOW)
        self.assertEqual(s['status'], 'ok')
        self.assertEqual(s['models'], [])
        self.assertTrue(all(value == 0 for value in s['totals'].values()))
        self.assertEqual(s['yesterday']['totals']['records'], MAX_MODELS + 1)
        self.assertEqual(s['yesterday']['totals']['cost'], (MAX_MODELS + 1) * .25)
        payload = encode_snapshot(s)
        self.assertNotIn(provider.encode(), payload)
        self.assertLess(len(payload), 1000)

    def test_yesterday_missing_price_and_partial(self):
        unpriced = event('unpriced', timestamp=NOW - 86400)
        unpriced['message']['usage'].pop('cost')
        self.save('session.jsonl', unpriced)
        s = self.scanner.snapshot(NOW)
        self.assertEqual(s['totals']['cost'], 0)
        self.assertEqual(s['yesterday']['status'], 'ok')
        self.assertIsNone(s['yesterday']['totals']['cost'])
        self.assertEqual(s['yesterday']['totals']['unpriced'], 1)
        path = self.save('session.jsonl', unpriced, event('priced', timestamp=NOW - 86400 - 10))
        with path.open('a') as stream: stream.write('{bad}\n')
        partial = self.scanner.snapshot(NOW)
        self.assertEqual(partial['status'], 'partial')
        self.assertEqual(partial['yesterday']['status'], 'partial')
        self.assertEqual(partial['yesterday']['issues'], 1)
        self.assertEqual(partial['yesterday']['totals']['cost'], .25)
        self.assertEqual(partial['yesterday']['totals']['unpriced'], 1)

    def test_yesterday_cache_rewrite_and_delete(self):
        a = event(timestamp=NOW - 86400)
        path = self.save('session.jsonl', a)
        self.assertEqual(self.scanner.snapshot(NOW)['yesterday']['totals']['cost'], .25)
        old = path.stat()
        a['message']['usage']['cost']['total'] = .75
        self.save('session.jsonl', a)
        os.utime(path, ns=(old.st_atime_ns, old.st_mtime_ns))
        self.assertEqual(path.stat().st_size, old.st_size)
        self.assertEqual(self.scanner.snapshot(NOW)['yesterday']['totals']['cost'], .75)
        path.unlink()
        s = self.scanner.snapshot(NOW)
        self.assertEqual(s['yesterday']['totals']['tokens'], 0)
        self.assertEqual(s['yesterday']['totals']['cost'], 0)
        self.assertEqual(s['yesterday']['status'], 'ok')
        self.assertFalse(self.scanner.cache)

    def test_yesterday_local_dst_days(self):
        os.environ['TZ'] = 'America/New_York'; time.tzset()
        for date, hours in [((2026, 3, 9), 23), ((2026, 11, 2), 25)]:
            with self.subTest(hours=hours):
                now = datetime(*date, 12).timestamp()
                _, start, _, _ = day_window(now)
                previous_date = datetime.fromtimestamp(now).date() - timedelta(days=1)
                previous_start = datetime.combine(previous_date, datetime.min.time()).timestamp()
                self.save('session.jsonl', event('before', timestamp=previous_start - .001),
                          event('first', timestamp=previous_start), event('last', timestamp=start - .001),
                          event('today', timestamp=start))
                s = self.scanner.snapshot(now)
                self.assertEqual(s['yesterday']['date'], previous_date.isoformat())
                self.assertEqual(s['yesterday']['totals']['tokens'], 32)
                self.assertEqual(s['yesterday']['totals']['cost'], .5)
                self.assertEqual(s['yesterday']['totals']['records'], 2)
                self.assertEqual(s['totals']['records'], 1)
                assert self.scanner.window is not None
                self.assertEqual(start - self.scanner.window[0], hours * 3600)

    def test_yesterday_date_when_timezone_skips_a_day(self):
        os.environ['TZ'] = 'Pacific/Apia'; time.tzset()
        now = datetime(2011, 12, 31, 12).timestamp()
        self.save('session.jsonl', event('old', timestamp=datetime(2011, 12, 29, 12).timestamp()),
                  event('today', timestamp=now - 1))
        s = self.scanner.snapshot(now)
        self.assertEqual(s['date'], '2011-12-31')
        self.assertEqual(s['totals']['records'], 1)
        self.assertEqual(s['yesterday']['date'], '2011-12-30')
        self.assertEqual(s['yesterday']['status'], 'ok')
        self.assertEqual(s['yesterday']['totals']['records'], 0)
        self.assertEqual(s['yesterday']['totals']['cost'], 0)

    def test_yesterday_overflow_does_not_discard_today(self):
        for field, value in [('input', MAX_COUNT), ('cost', float(MAX_COST))]:
            with self.subTest(field=field):
                a, b = event('y1', timestamp=NOW - 86400), event('y2', timestamp=NOW - 86400 - 1)
                for record in (a, b):
                    if field == 'cost':
                        record['message']['usage']['cost']['total'] = value
                    else:
                        record['message']['usage'].update(input=value, output=0, cacheRead=0, cacheWrite=0)
                self.save('session.jsonl', event('today'), a, b)
                s = self.scanner.snapshot(NOW)
                self.assertEqual(s['status'], 'ok')
                self.assertEqual(s['totals']['cost'], .25)
                self.assertEqual(s['totals']['records'], 1)
                self.assertEqual(s['yesterday']['status'], 'unavailable')
                self.assertGreaterEqual(s['yesterday']['issues'], 1)
                self.assertIsNone(s['yesterday']['totals']['cost'])
                self.assertTrue(all(v == 0 for k, v in s['yesterday']['totals'].items() if k != 'cost'))
                encode_snapshot(s)

    def test_today_fallback_preserves_yesterday_and_original_date(self):
        cases = [
            [event(str(i), model=f'm{i}') for i in range(MAX_MODELS + 1)],
            [event(model='m' * 193)],
            [event(str(i), provider='p' * 192, model=f'{i:03}' + 'm' * 189) for i in range(180)],
        ]
        for records in cases:
            with self.subTest(records=len(records)):
                self.save('session.jsonl', *records, event('yesterday', timestamp=NOW - 86400))
                original = self.scanner.snapshot(NOW)
                with self.assertRaises(ValueError): encode_snapshot(original)
                with patch.object(self.scanner, 'snapshot', return_value=original), \
                        patch('daemon.usage.clock.time', return_value=NOW + 86400):
                    s, payload = collect(self.scanner)
                self.assertEqual(s['status'], 'unavailable')
                self.assertEqual(s['epoch'], int(NOW))
                self.assertEqual(s['date'], '2026-10-16')
                self.assertEqual(s['yesterday'], original['yesterday'])
                self.assertEqual(s['yesterday']['status'], 'ok')
                self.assertEqual(s['yesterday']['totals']['cost'], .25)
                self.assertEqual(json.loads(payload), s)
                self.assertLessEqual(len(payload), MAX_FRAME_BYTES + 1)  # newline is the delimiter

    def test_unencodable_yesterday_falls_back_to_both_unavailable(self):
        s = self.scanner.snapshot(NOW)
        s['yesterday']['date'] = 'x' * MAX_FRAME_BYTES
        with patch.object(self.scanner, 'snapshot', return_value=s):
            fallback, payload = collect(self.scanner)
        self.assertEqual(fallback['status'], 'unavailable')
        self.assertEqual(fallback['yesterday']['status'], 'unavailable')
        self.assertEqual(fallback['yesterday']['date'], '2026-10-15')
        self.assertEqual(fallback['epoch'], int(NOW))
        self.assertEqual(json.loads(payload), fallback)

    def test_yesterday_bounds_legacy_fixture_and_once(self):
        s = self.scanner.snapshot(NOW)
        legacy = dict(s)
        del legacy['yesterday']
        self.assertEqual(json.loads(encode_snapshot(legacy)), legacy)
        for field, value in [('tokens', MAX_COUNT + 1), ('tokens', True), ('cost', float(MAX_COST) + 1)]:
            with self.subTest(field=field, value=value):
                invalid = json.loads(encode_snapshot(s))
                invalid['yesterday']['totals'][field] = value
                with self.assertRaises(ValueError): encode_snapshot(invalid)
        result = subprocess.run([sys.executable, '-m', 'daemon.tokenmeter', '--once', '--pi-dir', str(self.root)],
                                cwd=Path(__file__).resolve().parents[2], capture_output=True, text=True, check=True, timeout=10)
        once = json.loads(result.stdout)
        self.assertEqual(once['v'], 1)
        self.assertEqual(once['status'], 'ok')
        self.assertEqual(once['yesterday']['status'], 'ok')
        local_date = datetime.strptime(once['date'], '%Y-%m-%d').date()
        self.assertEqual(once['yesterday']['date'], (local_date - timedelta(days=1)).isoformat())
        self.assertLessEqual(len(encode_snapshot(once)), MAX_FRAME_BYTES + 1)

    def test_wire_bounds_and_utf8_identity(self):
        self.save('a.jsonl', event())
        s = self.scanner.snapshot(NOW)
        self.assertTrue(encode_snapshot(s).endswith(b'\n'))
        s['models'][0]['model'] = '界' * 65
        with self.assertRaises(ValueError): encode_snapshot(s)
        s = self.scanner.snapshot(NOW); s['models'] *= MAX_MODELS + 1
        with self.assertRaises(ValueError): encode_snapshot(s)
        s = self.scanner.snapshot(NOW); s['totals']['tokens'] = MAX_COUNT + 1
        with self.assertRaises(ValueError): encode_snapshot(s)
        self.save('a.jsonl', event(provider='bad\u0085identity', model=''))
        s = self.scanner.snapshot(NOW)
        self.assertEqual((s['models'][0]['provider'], s['models'][0]['model']), ('unknown', 'unknown'))
        encode_snapshot(s)

    def test_conflicting_copies_disclosed_and_invalid_cost_not_zero(self):
        a, b = event(), event(); a['message']['usage'].pop('cost')
        self.save('a.jsonl', a); self.save('b.jsonl', b)
        s = self.scanner.snapshot(NOW)
        self.assertEqual(s['totals']['records'], 1)
        self.assertEqual(s['totals']['cost'], .25)
        self.assertEqual(s['status'], 'partial')
        self.save('a.jsonl', event('different'))
        b['message']['usage']['cost']['total'] = float('nan'); self.save('b.jsonl', b)
        s = self.scanner.snapshot(NOW)
        self.assertEqual(s['totals']['unpriced'], 1)
        encode_snapshot(s)  # no NaN is exported


class TransportTests(unittest.IsolatedAsyncioTestCase):
    async def test_fragment_mtu_and_whole_frame_ack(self):
        queue = asyncio.Queue()
        writes = []
        class Client:
            mtu_size = 23
            async def write_gatt_char(self, uuid, data, response):
                self_test.assertEqual(uuid, RX_UUID)
                self_test.assertTrue(response)
                self_test.assertLessEqual(len(data), 20)
                writes.append(data)
                if data.endswith(b'\n'): queue.put_nowait(b'ok')
        self_test = self
        payload = ('{"name":"界","padding":"' + 'x'*200 + '"}\n').encode()
        queue.put_nowait(b'old')
        await write_snapshot(Client(), payload, queue)
        self.assertEqual(b''.join(writes), payload)

    async def test_device_rejection_is_not_success(self):
        queue = asyncio.Queue()
        class Client:
            mtu_size = 247
            async def write_gatt_char(self, _, data, response):
                if data.endswith(b'\n'): queue.put_nowait(b'error')
        with self.assertRaises(RuntimeError):
            await write_snapshot(Client(), b'{}\n', queue)


if __name__ == '__main__':
    unittest.main()
