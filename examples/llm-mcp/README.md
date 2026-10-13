# llm-mcp: a JIT-compiled C "hand" for LLMs

An MCP stdio server that lets an LLM compile and run native C through
TinyCC's `libtcc`, with typed return values, captured diagnostics, and
crash/timeout isolation. A typical round trip (compile + run) takes
about 10 ms.

The whole server is one C file, compiled **by TCC itself** together
with `libtcc.c` — the compiler compiling a server that embeds the
compiler. There is no interpreter or system compiler in the loop at
runtime.

```
MCP client ──JSON-RPC/stdio──> tcc-mcp-server ──fork──> JIT code
   (LLM)                       server.c + libtcc,      isolated;
                               built by ../../tcc      stdout piped
```

## Build and test

```sh
make            # in the repo root first, if tcc/libtcc1.a are not built
make -C examples/llm-mcp
make -C examples/llm-mcp test
```

The build rule is just:

```sh
../../tcc -B../.. -I../.. -o tcc-mcp-server server.c ../../libtcc.c -lm -ldl -lpthread
```

(`server.c` also compiles cleanly with `cc -Wall -Wextra` against
`libtcc.a`, which is handy for debugging, but the shipped binary is
the TCC-built one.)

## Run it

```sh
examples/llm-mcp/tcc-mcp-server
```

As an MCP client configuration (stdio transport):

```json
{
  "mcpServers": {
    "tcc": {
      "command": "/path/to/tinycc/examples/llm-mcp/tcc-mcp-server"
    }
  }
}
```

For OpenCode (`~/.config/opencode/opencode.json` or project `opencode.json`),
add this entry under `mcp`, then restart OpenCode:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "tcc": {
      "type": "local",
      "command": ["/path/to/tinycc/examples/llm-mcp/tcc-mcp-server"],
      "enabled": true
    }
  }
}
```

For Muse Code specifically, add this entry under `mcpServers` in
`${XDG_CONFIG_HOME:-$HOME/.config}/muse/settings.json`, then relaunch
Muse Code (note: a sandboxed Muse session cannot write that file
itself, since the config directory is deliberately outside its
writable set):

```json
"tcc": {
  "type": "stdio",
  "command": "/path/to/tinycc/examples/llm-mcp/tcc-mcp-server",
  "args": [],
  "mode": "optional"
}
```

The server finds `libtcc1.a` and headers relative to its own location
(`<server dir>/../..`). Environment overrides: `TCC_MCP_LIBPATH`
(directory holding `libtcc1.a`) and `TCC_MCP_STATE_DIR` (where
per-session host state is stored; defaults to a fresh temp directory).

## Tools

| Tool | Purpose |
|---|---|
| `c_eval` | Compile `code` and evaluate the `call` expression as `return_type` (`int`, `long`, `double`, `string`, or `void` statements) |
| `c_define` | Compile-check definitions, then append them to a session (rejected definitions leave the session unchanged) |
| `c_call` | `c_eval` with no new code, against earlier definitions |
| `c_reset` | Clear a session's definitions and host state |
| `c_sessions` | List sessions |

Every reply is a JSON object: `ok`, `status` (`ok`, `compile_error`,
`crashed`, `timed_out`, `exited`, ...), `result`, `stdout`,
`diagnostics`, `elapsed_ms`, and for crashes the `signal`.

Example `c_eval` reply:

```json
{"ok": true, "status": "ok", "result": 55, "stdout": "", "elapsed_ms": 9.2}
```

for `code = "int fib(int n){ return n<=2 ? 1 : fib(n-1)+fib(n-2); }"`,
`call = "fib(10)"`.

Example prompts to give the model once connected: "use c_eval to check
`sizeof` and field offsets of this struct on this machine", "write a C
helper that parses this binary format and call it on the header bytes",
"benchmark these two implementations with host_now_ms()".

## How it works

- **No FFI.** Typed calls are produced by generating a C wrapper,
  e.g. `long long __mcp_entry(void){ return (long long)(fib(10)); }`,
  so TCC itself typechecks and marshals the arguments. The server then
  calls `__mcp_entry` via `tcc_get_symbol`.
- **Isolation.** The server forks before calling JIT code; the child
  runs under `RLIMIT_CPU` with core dumps disabled and stdin from
  `/dev/null`, its stdout/stderr piped back, under a wall-clock
  deadline after which the child's process group is killed.
  Segfaults and infinite loops are reported as data, never fatal.
- **Sessions persist definitions, not globals.** Two probes against
  this tree shaped the design: `tcc_compile_string` after
  `tcc_relocate` segfaults (AArch64/Darwin), and `-b` bounds checking
  crashes in `TCC_OUTPUT_MEMORY` mode even for clean code. So each
  call recompiles the session's accumulated definitions from scratch
  (TCC is fast enough that this is milliseconds), and values that must
  survive between calls go through host functions backed by a
  per-session file: `host_set_int`/`host_get_int`,
  `host_set_double`/`host_get_double`, `host_set_str`/`host_get_str`,
  `host_has`, `host_now_ms`. These are predeclared, along with
  `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<stdint.h>`
  and `<stdbool.h>`, and committed only when the JIT child exits
  cleanly — so state also survives crashes.
- Diagnostics use `#line` directives, so compiler messages name
  `session.c`, `eval.c` or `call.c` with line numbers local to the
  chunk the model wrote. Compiler errors are captured through
  `tcc_set_error_func`.
- The JSON reader/writer in `server.c` is deliberately minimal: it
  locates object members by key, decodes the scalars MCP needs
  (including `\uXXXX` escapes), and batches contiguous unescaped string
  spans when reading and writing JSON payloads.
- Client quirks handled: the server auto-detects newline-delimited
  JSON versus LSP-style `Content-Length` framing per message and
  replies in kind, and it clears any `O_NONBLOCK` flag an async
  client runtime leaks into its stdin/stdout at spawn. Verified
  with OpenCode (`mcp` in `opencode.json`), Muse (`mcpServers` in
  `settings.json`), and Grok (`grok mcp add tcc -- /path/to/tcc-mcp-server`,
  then `grok mcp doctor tcc`).

## Closed-Loop JIT Introspection & Optimization

Because functions compiled in a session live directly in executable RAM
(`TCC_OUTPUT_MEMORY`) and every invocation runs in a crash-isolated
`fork()` child, an LLM can use `tcc-mcp-server` as a live
machine-code and hardware oracle:

1. **Self-disassembly in RAM:** Cast any compiled function pointer
   (`(const uint32_t *)&fn`) to inspect the exact instruction stream,
   stack spills, and code size TCC emitted without writing object files.
2. **Crash-safe hardware & kernel fault probing:** Deliberately execute
   privileged registers (`mrs` on EL1 -> `SIGILL`), W^X `MAP_JIT`
   write-while-executable violations (`SIGBUS`), or NX page fetches
   (`SIGBUS`) and receive structured JSON signal reports in ~15-40 ms.
3. **Closed-loop compiler & C optimization:** Prototype C rewrites,
   inline assembly, or `MAP_JIT` machine code, verify bitwise and CPU
   flag (`NZCV`) equivalence across edge cases, and benchmark in-process
   with `host_now_ms()`.

### Optimizations driven by `llm-mcp`

| Component | Change | Measured Result |
|---|---|---|
| `examples/llm-mcp/server.c` (`json_escape`, `json_decode_string`) | Batch contiguous unescaped spans via `str_addn` instead of per-byte `str_addc` | **4.30x faster** (`494 ms -> 115 ms` on 4 KB x 20k) |
| `tccelf.c` (`elf_hash`) | Branchless single-dereference loop (`h ^= (h >> 24) & 0xf0; h &= 0x0fffffff`) | **2.01x faster** (`51 -> 41` insns, `196 ns -> 97.5 ns/call`) |
| `arm64-asm.c` (`asm_bitop`) | Implement `rbit`, `clz`, `cls`, `rev`, `rev16`, `rev32`, `rev64` inline assembly opcodes | **6.21x faster** 32-bit bit reversal (`rbit` vs 5-stage SWAR) |
| `lib/builtin.c` (`libtcc1.a`) | Use ARM64 `clz`, `rbit`+`clz`, and `cls` for `__builtin_clz*`, `__builtin_ctz*`, `__builtin_clrsb*`, and `__builtin_ffs*`; omit static De Bruijn tables on AArch64 | **2.29x faster** (`clz+ctz+clrsb`), **1.29x faster** (`ffs`), `3x` smaller code |
| `arm64-gen.c` (`arm64_gen_opic`) | Emit immediate `cmp`/`cmn [wx]a, #imm12` for `TOK_EQ..TOK_UGT`; strength-reduce `* 0`, `* 1`, `* -1`, and `* ((2^k + 1) << s)` (`* 3, 5, 6, 9, 10, 12, 17, 33...`) to shifted `add`/`lsl`; fix missing destination register `\| x` on `val == 0` shifts | **28% fewer instructions** in decimal parsing loops (`43 -> 31` insns); fixes `<< 32` register clobber |
| `tccpp.c` (`TOK_HASH_FUNC`, `tok_alloc`) | Express `h + (h << 5)` as `h * 33u` (paired with `arm64_gen_opic` shifted `add`) and pointer-walk `p < end` | **1.15x faster** identifier hashing, 3 fewer stack loads/char |

## Security

This is crash isolation, **not** a security sandbox: JIT code runs
with the server process's privileges and can call any library function
or syscall — that is the point of giving the model a "hand". Only
connect MCP clients and models you trust with code execution on this
machine, exactly as you would for a shell tool. If you need a harder
boundary, run the server inside a VM/container or under an OS sandbox
profile, and consider it a starting point rather than a guarantee.

## Limitations

- C globals and `static` locals reset every call; use the `host_*`
  functions for persistent values (keys: 63 chars max, 256 entries).
- If JIT code calls `exit()`, the call reports `exited` with the code
  and uncommitted host state is lost.
- `string` results are copied out at return; returning a pointer to
  freed or stack memory is as undefined as in ordinary C. Results are
  truncated at 8 KiB, stdout at 64 KiB.
- One call at a time per server process (calls are milliseconds; run
  several servers for parallelism). `libtcc_test_mt.c` shows the
  multithreaded direction if this grows up.
- `__mcp_entry` is a reserved name.

## Files

- `server.c` — the entire server: JSON-RPC, sessions, `libtcc`
  compile + forked execution, host key/value state
- `Makefile` — builds `tcc-mcp-server` with `../../tcc`
- `test_mcp.py` — end-to-end tests driving the binary as an MCP
  client (Python is used only as the test harness)
