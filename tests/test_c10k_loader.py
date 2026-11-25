"""Small real-TCP fixtures for heartbeat scheduling and fail-closed reuse."""
import asyncio
from types import SimpleNamespace
import unittest
import check_c10k as loader


class LoaderTests(unittest.TestCase):
    def test_capacity_clock_tracks_full_intervals_and_released_slots(self):
        clock = loader.CapacityClock(2, 0)
        first = clock.admit(1)
        second = clock.admit(2)  # Full during [2, 4].
        clock.release(first, 4)
        third = clock.admit(5)  # Full during [5, 9].
        clock.release(second, 9)
        clock.release(second, 10)  # Delayed task callback must not double-release.
        self.assertEqual(clock.active, 1)
        self.assertAlmostEqual(clock.full_time_at(1), 0)
        self.assertAlmostEqual(clock.full_time_at(3), 1)
        self.assertAlmostEqual(clock.full_time_at(7), 4)
        self.assertAlmostEqual(clock.full_time_at(12), 6)
        # A request due at 3 and admitted at 8 waits full=4, available=1.
        full = clock.full_time_at(8) - clock.full_time_at(3)
        self.assertAlmostEqual(full, 4)
        self.assertAlmostEqual((8 - 3) - full, 1)
        clock.release(third, 12)
        self.assertEqual(clock.active, 0)

    def setUp(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)

    def tearDown(self):
        self.loop.run_until_complete(self.loop.shutdown_asyncgens())
        self.loop.close()

    def test_refreshes_old_connections_while_ramping(self):
        async def scenario():
            active, events, handlers = set(), [], set()
            async def handler(reader, writer):
                task = asyncio.current_task() if hasattr(asyncio, 'current_task') else asyncio.Task.current_task()
                handlers.add(task); active.add(writer)
                try:
                    while True:
                        header = await reader.readuntil(b'\r\n\r\n')
                        length = int([x for x in header.split(b'\r\n') if x.lower().startswith(b'content-length:')][0].split(b':')[1])
                        body = await reader.readexactly(length)
                        index, sequence = map(int, body.split(b':'))
                        events.append((index, sequence))
                        await asyncio.sleep(.02 if sequence == 0 else .001)
                        writer.write(b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(length).encode() +
                                     b'\r\nConnection: keep-alive\r\nVia: 1.1 jungle-proxy\r\n\r\n' + body)
                        await writer.drain()
                except (asyncio.IncompleteReadError, ConnectionError):
                    pass
                finally:
                    active.remove(writer); writer.close(); handlers.remove(task)
            server = await asyncio.start_server(handler, '127.0.0.1', 0)
            target = server.sockets[0].getsockname()[1]
            original = loader.snapshot_counts
            async def snapshot(*a):
                return dict(frontend=len(active), backend=len(active))
            loader.snapshot_counts = snapshot
            observation = {}
            try:
                args = SimpleNamespace(connections=20, concurrency=2, heartbeat=.05,
                    max_gap=1, hold=.15, total_timeout=5, request_timeout=1, nonempty_body=True)
                await loader.retain(args, target, 0, 0, observation)
                first_reuse = next(i for i, event in enumerate(events) if event[1] > 0)
                final_new = next(i for i, event in enumerate(events) if event == (19, 0))
                self.assertLess(first_reuse, final_new)
                self.assertEqual(observation['tcp_opens'], 20)
                self.assertLessEqual(observation['max_in_flight_requests'], 2)
                self.assertGreaterEqual(observation['min_responses_per_connection'], 2)
                self.assertGreaterEqual(len(observation['snapshots']), 2)
                metrics = observation['request_timing']
                for kind in ('first', 'reuse'):
                    total = metrics[kind + '_schedule_to_send_seconds']
                    parts = [metrics[kind + '_' + name + '_seconds'] for name in
                             ('slot_full_wait', 'slot_available_wait', 'admission_to_send')]
                    self.assertAlmostEqual(total['total'], sum(p['total'] for p in parts), places=7)
                    for part in parts:
                        self.assertEqual(part['count'], total['count'])
                        self.assertGreaterEqual(part['total'], 0)
                        self.assertGreaterEqual(part['maximum'], 0)
                self.assertGreater(observation['capacity_timing']['full_seconds'], 0)
            finally:
                loader.snapshot_counts = original
                server.close(); await server.wait_closed()
                for writer in list(active): writer.close()
                if handlers: await asyncio.gather(*list(handlers), return_exceptions=True)
        self.loop.run_until_complete(scenario())

    def test_closed_connection_is_not_reopened(self):
        async def scenario():
            async def handler(reader, writer):
                await reader.readuntil(b'\r\n\r\n')
                body = await reader.readexactly(3)
                writer.write(b'HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: keep-alive\r\nVia: 1.1 jungle-proxy\r\n\r\n' + body)
                await writer.drain(); writer.close()
            server = await asyncio.start_server(handler, '127.0.0.1', 0)
            client = loader.Client(0, empty_body=False)
            stats = dict(tcp_opens=0, initialized=0, requests=0)
            capacity = loader.CapacityClock(1, loader.time.monotonic())
            try:
                await client.exchange('127.0.0.1', server.sockets[0].getsockname()[1], stats,
                                      capacity=capacity, slot=capacity.admit())
                self.assertEqual(capacity.active, 0)
                with self.assertRaises((asyncio.IncompleteReadError, ConnectionError)):
                    await client.exchange('127.0.0.1', server.sockets[0].getsockname()[1], stats,
                                          capacity=capacity, slot=capacity.admit())
                self.assertEqual(capacity.active, 0)
                self.assertEqual(stats['tcp_opens'], 1)
                self.assertEqual(stats['requests'], 1)
            finally:
                client.close(); server.close(); await server.wait_closed()
                await asyncio.sleep(0)
        self.loop.run_until_complete(scenario())


if __name__ == '__main__': unittest.main(verbosity=2)
