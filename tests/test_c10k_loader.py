"""Small real-TCP fixtures for heartbeat scheduling and fail-closed reuse."""
import asyncio
from types import SimpleNamespace
import unittest
import check_c10k as loader


class LoaderTests(unittest.TestCase):
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
            try:
                await client.exchange('127.0.0.1', server.sockets[0].getsockname()[1], stats)
                with self.assertRaises((asyncio.IncompleteReadError, ConnectionError)):
                    await client.exchange('127.0.0.1', server.sockets[0].getsockname()[1], stats)
                self.assertEqual(stats['tcp_opens'], 1)
                self.assertEqual(stats['requests'], 1)
            finally:
                client.close(); server.close(); await server.wait_closed()
                await asyncio.sleep(0)
        self.loop.run_until_complete(scenario())


if __name__ == '__main__': unittest.main(verbosity=2)
