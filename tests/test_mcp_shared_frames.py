"""Hermetic broker framing fixtures; no live sockets, hosts, or signals."""
import asyncio
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import AsyncMock, Mock, patch

spec = importlib.util.spec_from_file_location("shared_fixture", Path(__file__).resolve().parents[1]/"tools/cnet_mcp_shared.py")
broker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(broker)


class FrameTests(unittest.IsolatedAsyncioTestCase):
    async def test_backend_pipes_accept_bounded_inventory_above_64k(self):
        host = broker.SharedDaemon()
        process = Mock(stdin=Mock(), stdout=Mock(), stderr=Mock())
        host._probe_ready = AsyncMock()
        async def idle():
            await asyncio.Event().wait()
        host._backend_stdout_loop = idle
        host._backend_stderr_loop = idle
        host._backend_watch_exit = idle
        with patch.object(broker, "SERVER_BIN", Path(sys.executable)), \
             patch.object(broker, "_log"), \
             patch.object(asyncio, "create_subprocess_exec", AsyncMock(return_value=process)) as spawn:
            await host.start_backend()
            self.assertGreaterEqual(spawn.call_args.kwargs.get("limit", 65536), 2*1024**2,
                                    "MCP_BROKER_FRAME_RED")

    async def run_reader(self, data, limit=2*1024**2, eof=True, original_id=17):
        host = broker.SharedDaemon()
        reader = asyncio.StreamReader(limit=limit)
        reader.feed_data(data)
        if eof: reader.feed_eof()
        host._backend = Mock(stdout=reader)
        writer = Mock(drain=AsyncMock())
        host._pending['"fixture"'] = (writer, original_id)
        host._pending['"second"'] = (writer, 18)
        with patch.object(broker.os, "kill") as kill, patch.object(broker, "_log"):
            await asyncio.wait_for(host._backend_stdout_loop(), .5)
        return writer, kill

    async def test_bounded_response_preserves_original_id_and_next_response(self):
        response = dict(jsonrpc="2.0", id="fixture", result="x"*100000)
        data = (json.dumps(response)+'\n'+json.dumps(dict(jsonrpc='2.0',id='second',result='next'))+'\n').encode()
        writer, kill = await self.run_reader(data)
        self.assertEqual(len(writer.write.call_args_list), 2)
        first = json.loads(writer.write.call_args_list[0].args[0])
        self.assertEqual(first["id"], 17)
        self.assertEqual(len(first["result"]), 100000)
        self.assertEqual(json.loads(writer.write.call_args_list[1].args[0])["id"], 18)

    async def test_oversized_backend_frame_fails_loud_not_silent_dead_reader(self):
        writer, kill = await self.run_reader(b"x"*(2*1024**2)+b"\n", eof=False)
        self.assertTrue(kill.called, "MCP_BROKER_DEAD_READER_RED")
        writer.write.assert_not_called()

    async def test_malformed_partial_and_non_object_responses_fail_loud(self):
        for data in [b'{"jsonrpc":"2.0","id":"fixture"}', b'[]\n', b'invalid\n',
                     b'{"jsonrpc":"2.0","id":"fixture","result":"\xff"}\n',
                     b'{"id":"fixture","result":'+b'['*10000+b'0'+b']'*10000+b'}\n']:
            writer, kill = await self.run_reader(data, eof=not data.endswith(b'\n'))
            self.assertTrue(kill.called, "MCP_BROKER_INVALID_FRAME_RED")
            writer.write.assert_not_called()

    async def test_exact_frame_bound_and_outbound_id_growth(self):
        prefix = b'{"jsonrpc":"2.0","id":"fixture","result":"'
        suffix = b'"}\n'
        data = prefix+b'x'*(2*1024**2-len(prefix)-len(suffix))+suffix
        writer, _ = await self.run_reader(data)
        self.assertIn('result', json.loads(writer.write.call_args.args[0]))
        writer, _ = await self.run_reader(data, original_id='y'*110)
        output = writer.write.call_args.args[0]
        self.assertLessEqual(len(output), 2*1024**2, 'MCP_BROKER_OUTBOUND_RED')
        self.assertEqual(json.loads(output)['error']['message'], 'response_frame_exceeds_limit')


if __name__ == "__main__":
    unittest.main()
