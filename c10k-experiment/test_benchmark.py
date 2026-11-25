import asyncio
import unittest
from benchmark import read_response, percentile, wait_process
from unittest.mock import Mock


class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def test_fragmented_response_and_next_response(self):
        reader = asyncio.StreamReader()
        reader.feed_data(b'HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ncat!HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n')
        self.assertEqual(await read_response(reader), (200, b'cat!'))
        self.assertEqual(await read_response(reader), (200, b''))

    async def test_truncated_body_fails(self):
        reader = asyncio.StreamReader()
        reader.feed_data(b'HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nca')
        reader.feed_eof()
        with self.assertRaises(asyncio.IncompleteReadError):
            await read_response(reader)

    def test_percentile(self):
        self.assertEqual(percentile(list(range(1, 101)), .95), 95)

    async def test_wait_process_without_worker_thread(self):
        process = Mock()
        process.poll.side_effect = [None, 0]
        self.assertTrue(await wait_process(process, .5))
        process.poll.side_effect = None
        process.poll.return_value = None
        self.assertFalse(await wait_process(process, 0))


if __name__ == '__main__':
    unittest.main()
