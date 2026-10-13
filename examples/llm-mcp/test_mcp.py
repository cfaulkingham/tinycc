#!/usr/bin/env python3
"""End-to-end tests: drive the TCC-built tcc-mcp-server as a real MCP
stdio client would. (Python here is only the test harness; the server
itself is C compiled by TCC.)

Run with `make test` (builds the server first) or:
    python3 -m unittest -v test_mcp
"""

import json
import math
import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


class McpClient:
    def __init__(self):
        self.proc = subprocess.Popen(
            [str(HERE / "tcc-mcp-server")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.next_id = 0

    def send(self, obj):
        self.proc.stdin.write(json.dumps(obj) + "\n")
        self.proc.stdin.flush()

    def request(self, method, params=None):
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": self.next_id, "method": method,
                   "params": params or {}})
        line = self.proc.stdout.readline()
        assert line, f"server closed stdout (rc={self.proc.poll()})"
        return json.loads(line)

    def call(self, tool, **args):
        resp = self.request("tools/call", {"name": tool, "arguments": args})
        assert "result" in resp, resp
        return json.loads(resp["result"]["content"][0]["text"])

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)
        self.proc.stdout.close()


class TccMcpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(HERE)], check=True, capture_output=True)

    def setUp(self):
        self.mcp = McpClient()
        init = self.mcp.request("initialize", {"protocolVersion": "2025-03-26",
                                               "capabilities": {}, "clientInfo": {"name": "test"}})
        self.assertEqual(init["result"]["serverInfo"]["name"], "tcc-mcp")
        self.mcp.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def tearDown(self):
        self.mcp.close()

    def test_tools_list(self):
        tools = self.mcp.request("tools/list")["result"]["tools"]
        self.assertEqual([t["name"] for t in tools],
                         ["c_eval", "c_define", "c_call", "c_reset", "c_sessions"])

    def test_eval_int(self):
        r = self.mcp.call("c_eval",
                          code="int fib(int n){return n<=2?1:fib(n-1)+fib(n-2);}",
                          call="fib(10)")
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["result"], 55)

    def test_eval_double_and_string(self):
        r = self.mcp.call("c_eval", code="", call="sqrt(2.0)", return_type="double")
        self.assertAlmostEqual(r["result"], math.sqrt(2.0), places=12)
        r = self.mcp.call("c_eval", code='const char *who(void){return "tcc\\n";}',
                          call="who()", return_type="string")
        self.assertEqual(r["result"], "tcc\n")

    def test_eval_void_statements_capture_stdout(self):
        r = self.mcp.call("c_eval", code="",
                          call='for (int i=0;i<3;i++) printf("i=%d\\n", i);',
                          return_type="void")
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["stdout"], "i=0\ni=1\ni=2\n")

    def test_long_return_not_truncated_to_int(self):
        r = self.mcp.call("c_eval", code="", call="(1LL<<40)+5", return_type="long")
        self.assertEqual(r["result"], (1 << 40) + 5)

    def test_compile_error_is_reported(self):
        r = self.mcp.call("c_eval", code="int broken(void){ return 1 + ; }", call="broken()")
        self.assertFalse(r["ok"])
        self.assertEqual(r["status"], "compile_error")
        self.assertIn("error", r["diagnostics"])
        self.assertIn("eval.c", r["diagnostics"])  # #line keeps names meaningful

    def test_crash_is_isolated(self):
        r = self.mcp.call("c_eval", code="", call="*(volatile int *)0")
        self.assertFalse(r["ok"])
        self.assertEqual(r["status"], "crashed")
        self.assertEqual(r["signal"], "SIGSEGV")
        r = self.mcp.call("c_eval", code="", call="20+22")  # server still alive
        self.assertEqual(r["result"], 42)

    def test_timeout_is_isolated(self):
        r = self.mcp.call("c_eval", code="", call="while (1) { }",
                          return_type="void", timeout_ms=300)
        self.assertFalse(r["ok"])
        self.assertEqual(r["status"], "timed_out")
        r = self.mcp.call("c_eval", code="", call="7*6")
        self.assertEqual(r["result"], 42)

    def test_define_then_call_and_bad_define_rolls_back(self):
        r = self.mcp.call("c_define", code="int square(int x){return x*x;}", session="s1")
        self.assertTrue(r["ok"], r)
        self.assertTrue(any(name.endswith("square") for name in r["symbols"]), r)
        r = self.mcp.call("c_define", code="int nope( {", session="s1")
        self.assertFalse(r["ok"])
        r = self.mcp.call("c_call", call="square(9)", session="s1")
        self.assertEqual(r["result"], 81)

    def test_host_state_persists_across_calls(self):
        self.mcp.call("c_define", session="ctr",
                      code=("long long bump(void){"
                            " host_set_int(\"n\", host_get_int(\"n\")+1);"
                            " return host_get_int(\"n\"); }"))
        self.assertEqual(self.mcp.call("c_call", call="bump()", session="ctr",
                                       return_type="long")["result"], 1)
        self.assertEqual(self.mcp.call("c_call", call="bump()", session="ctr",
                                       return_type="long")["result"], 2)
        self.mcp.call("c_reset", session="ctr")
        self.mcp.call("c_define", session="ctr",
                      code="long long bump(void){ return host_get_int(\"n\"); }")
        self.assertEqual(self.mcp.call("c_call", call="bump()", session="ctr",
                                       return_type="long")["result"], 0)

    def test_content_length_framing(self):
        """Some clients (Grok) use LSP-style Content-Length framing."""
        self.mcp.close()
        proc = subprocess.Popen([str(HERE / "tcc-mcp-server")],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            def framed(obj):
                body = json.dumps(obj).encode()
                proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
                proc.stdin.flush()
                header = proc.stdout.readline().decode()
                assert header.lower().startswith("content-length:"), header
                length = int(header.split(":")[1])
                assert proc.stdout.readline() == b"\r\n"
                return json.loads(proc.stdout.read(length))

            init = framed({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                           "params": {"protocolVersion": "2025-03-26"}})
            self.assertEqual(init["result"]["serverInfo"]["name"], "tcc-mcp")
            resp = framed({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                           "params": {"name": "c_eval", "arguments": {"call": "6*7"}}})
            payload = json.loads(resp["result"]["content"][0]["text"])
            self.assertEqual(payload["result"], 42)
        finally:
            proc.stdin.close()
            proc.wait(timeout=10)
            proc.stdout.close()
        self.setUp()  # tearDown closes this fresh client

    def test_nonblocking_stdin_is_cleared(self):
        """Async clients (Grok/tokio) can leak O_NONBLOCK into our stdin."""
        import os
        self.mcp.close()
        proc = subprocess.Popen(
            [str(HERE / "tcc-mcp-server")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
            preexec_fn=lambda: os.set_blocking(0, False))
        try:
            os.set_blocking(proc.stdout.fileno(), True)
            proc.stdin.write(json.dumps(
                {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                 "params": {"protocolVersion": "2025-03-26"}}) + "\n")
            proc.stdin.flush()
            resp = json.loads(proc.stdout.readline())
            self.assertEqual(resp["result"]["serverInfo"]["name"], "tcc-mcp")
        finally:
            proc.stdin.close()
            proc.wait(timeout=10)
            proc.stdout.close()
        self.setUp()  # tearDown closes this fresh client

    def test_sessions_listing(self):
        self.mcp.call("c_eval", code="", call="1", session="alpha")
        names = [s["name"] for s in self.mcp.call("c_sessions")["sessions"]]
        self.assertIn("alpha", names)


if __name__ == "__main__":
    unittest.main()
