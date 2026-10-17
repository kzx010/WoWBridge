"""Exercise service CLI logging choices through real loopback requests."""
import asyncio
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from service import server as entry
from service.catalog import DEFAULT as CATALOG


class ServiceCLI(unittest.IsolatedAsyncioTestCase):
    async def roundtrip(self, root, trace=False):
        real_start = asyncio.start_server
        real_service = entry.Service
        ready = asyncio.Event()
        servers, services = [], []

        async def listen(*args, **kwargs):
            instance = await real_start(*args, **kwargs)
            servers.append(instance)
            ready.set()
            return instance

        def service(*args, **kwargs):
            instance = real_service(*args, **kwargs)
            services.append(instance)
            return instance

        argv = ['service.server', '--catalog', str(CATALOG), '--port', '0']
        if trace:
            argv += ['--trace', str(root / 'explicit.jsonl')]
        with patch.object(sys, 'argv', argv), patch.object(entry, 'DEFAULT', root / 'catalog.json'), \
                patch.object(entry.asyncio, 'start_server', side_effect=listen), \
                patch.object(entry, 'Service', side_effect=service), contextlib.redirect_stdout(io.StringIO()):
            task = asyncio.create_task(entry.main())
            writer = None
            try:
                await asyncio.wait_for(ready.wait(), 5)
                reader, writer = await asyncio.open_connection('127.0.0.1', servers[0].sockets[0].getsockname()[1])
                writer.write(b'{"op":"hello","id":7}\n')
                await writer.drain()
                reply = json.loads(await asyncio.wait_for(reader.readline(), 5))
                self.assertTrue(reply['ok'])
                self.assertEqual(reply['id'], 7)
            finally:
                if writer:
                    writer.close()
                    await writer.wait_closed()
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await task
                for instance in services:
                    if instance.trace:
                        instance.trace.close()
        return services[0]

    async def test_default_start_serves_requests_without_recording_game_state(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            svc = await self.roundtrip(root)
            self.assertIsNone(svc.trace)
            self.assertEqual(list(root.iterdir()), [])

    async def test_explicit_trace_appends_without_removing_existing_records(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            path = root / 'explicit.jsonl'
            path.write_text('{"earlier":true}\n')
            await self.roundtrip(root, trace=True)
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            self.assertEqual(rows[0], {'earlier': True})
            self.assertEqual(len(rows), 2)
            self.assertEqual(json.loads(rows[1]['req']), {'op': 'hello', 'id': 7})
            self.assertEqual(rows[1]['res']['id'], 7)
