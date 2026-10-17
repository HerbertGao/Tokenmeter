import asyncio
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from daemon.usage import UsageScanner, day_window, encode_snapshot, MAX_COUNT, MAX_MODELS
from daemon.tokenmeter import RX_UUID, write_snapshot

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
        self.assertEqual(self.scanner.snapshot(NOW)['totals']['cost'], 0)
        missing = UsageScanner([self.root / 'missing']).snapshot(NOW)
        self.assertEqual(missing['status'], 'unavailable')
        self.assertIsNone(missing['totals']['cost'])
        self.assertEqual(UsageScanner([self.root, self.root / 'missing']).snapshot(NOW)['status'], 'partial')

    def test_missing_splits_fallback_and_malformed_complete_vs_unfinished_line(self):
        a = event(); a['message']['usage'] = dict(totalTokens=99, cost=dict(total=.5))
        path = self.save('a.jsonl', a)
        with path.open('a') as f: f.write('{"unfinished":')
        snapshot = self.scanner.snapshot(NOW)
        self.assertEqual(snapshot['totals']['tokens'], 99)
        self.assertEqual(snapshot['issues'], 1)  # missing splits, not the unfinished tail
        with path.open('a') as f: f.write('bad}\n')
        self.assertEqual(self.scanner.snapshot(NOW)['issues'], 2)

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
