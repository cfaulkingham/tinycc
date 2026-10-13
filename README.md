# Tiny C Compiler (TCC) + LLM MCP Server

This repository extends upstream [TinyCC (`repo.or.cz/w/tinycc.git`)](https://repo.or.cz/w/tinycc.git) with **`examples/llm-mcp`** — a self-hosted Model Context Protocol (MCP) stdio server that gives LLM agents a ~10 ms JIT-compiled C execution environment via `libtcc` — along with ARM64 code generator, inline assembler, lexer, ELF linker, and `libtcc1.a` runtime optimizations discovered and verified through the MCP server itself.

---

## Updates Over Upstream (`https://repo.or.cz/w/tinycc.git`)

### 1. `examples/llm-mcp`: JIT-Compiled C MCP Server (`tcc-mcp-server`)
An MCP stdio server written in single-file C (`examples/llm-mcp/server.c`) and compiled **by TCC itself** together with `libtcc.c`. There is no external compiler or interpreter in the loop at runtime.

- **Sub-20 ms compile + run:** Compiles C source directly into memory (`TCC_OUTPUT_MEMORY`) and invokes a typed wrapper (`int`, `long`, `double`, `string`, or `void` statements with captured `stdout`).
- **Crash & timeout isolation:** Every JIT execution runs in a `fork()`ed child process under `RLIMIT_CPU` and a wall-clock timeout. Segfaults (`SIGSEGV`), bus errors (`SIGBUS`), illegal instructions (`SIGILL`), and infinite loops are caught and returned as structured JSON without taking down the server or losing session state.
- **Stateful sessions (`c_define` / `c_call`) & host KV store:** Accumulate validated C definitions across turns and persist state across calls via pre-declared host helpers:
  - `host_get_int(key)` / `host_set_int(key, val)`
  - `host_get_double(key)` / `host_set_double(key, val)`
  - `host_get_str(key)` / `host_set_str(key, val)`
  - `host_has(key)` / `host_now_ms()`

### 2. Compiler, Assembler & Runtime Improvements
| Component | Update over `repo.or.cz/w/tinycc.git` | Impact |
|---|---|---|
| **`arm64-gen.c` (`arm64_gen_opic`)** | Emit immediate `cmp`/`cmn [wx]a, #imm12` for constant comparisons (`==, !=, <, <=, >, >=`); strength-reduce constant multiplies by `0`, `1`, `-1`, and `(2^k + 1) << s` (`* 3, 5, 6, 9, 10, 12, 17, 33...`) to single-cycle shifted `add`/`lsl`; fix destination register clobber (`\| x`) on `val == 0` shifts | **28% fewer instructions** in decimal parsing loops; eliminates scratch register on constant comparisons & multiplies; fixes `<< 32` clobber bug |
| **`arm64-asm.c` (`asm_bitop`)** | Implement ARMv8-A bit-manipulation inline assembly instructions: `rbit`, `clz`, `cls`, `rev`, `rev16`, `rev32`, `rev64` | **6.21x faster** 32-bit bit reversal (`rbit` vs 5-stage SWAR) |
| **`lib/builtin.c` (`libtcc1.a`)** | Use hardware ARM64 `clz`, `rbit`+`clz`, and `cls` for `__builtin_clz*`, `__builtin_ctz*`, `__builtin_clrsb*`, and `__builtin_ffs*`; omit static De Bruijn tables on AArch64 | **2.29x faster** (`clz`/`ctz`/`clrsb`), **1.29x faster** (`ffs`), **3x smaller** code |
| **`tccelf.c` (`elf_hash`)** | Branchless, single-dereference ELF symbol hash (`h ^= (h >> 24) & 0xf0; h &= 0x0fffffff`) | **2.01x faster** symbol hashing, `20%` smaller function |
| **`tccpp.c` (`TOK_HASH_FUNC`, `tok_alloc`)** | Express `h + (h << 5)` as `h * 33u` (lowered to a single `add w0, w0, w0, lsl #5` on ARM64) and pointer-walk `p < end` | **1.15x faster** identifier hashing, 3 fewer stack loads per byte |
| **`examples/llm-mcp/server.c`** | Span-batched `json_escape` and `json_decode_string` via `str_addn` | **4.30x faster** JSON string serialization |

---

## Compiling TinyCC and the MCP Server

### 1. Build TinyCC and `libtcc1.a`
```sh
./configure
make
make test
```

### 2. Build and Test `tcc-mcp-server`
```sh
make -C examples/llm-mcp
make -C examples/llm-mcp test
```
Under the hood, `examples/llm-mcp/Makefile` builds the self-contained MCP binary using the freshly built `./tcc`:
```sh
../../tcc -B../.. -I../.. -o tcc-mcp-server server.c ../../libtcc.c -lm -ldl -lpthread
```

---

## Setting Up and Using the MCP Server

`tcc-mcp-server` speaks MCP over `stdio` (auto-detecting both newline-delimited JSON-RPC and `Content-Length` framed messages). It locates `libtcc1.a` and standard headers relative to its own binary path (`<server_dir>/../..`), or via `TCC_MCP_LIBPATH`.

### OpenCode (`~/.config/opencode/opencode.json` or `./opencode.json`)
Add the `"tcc"` entry under `"mcp"`, then quit and restart OpenCode:
```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "tcc": {
      "type": "local",
      "command": [
        "/absolute/path/to/tinycc/examples/llm-mcp/tcc-mcp-server"
      ],
      "enabled": true
    }
  }
}
```

### Claude Code / Generic MCP Clients (`mcpServers`)
```json
{
  "mcpServers": {
    "tcc": {
      "command": "/absolute/path/to/tinycc/examples/llm-mcp/tcc-mcp-server"
    }
  }
}
```

### Muse Code (`~/.config/muse/settings.json`)
```json
"tcc": {
  "type": "stdio",
  "command": "/absolute/path/to/tinycc/examples/llm-mcp/tcc-mcp-server",
  "args": [],
  "mode": "optional"
}
```

### Grok CLI
```sh
grok mcp add tcc -- /absolute/path/to/tinycc/examples/llm-mcp/tcc-mcp-server
grok mcp doctor tcc
```

---

## MCP Tools Reference

| Tool | Arguments | Description |
|---|---|---|
| `c_eval` | `call` (req), `code`, `return_type` (`int`\|`long`\|`double`\|`string`\|`void`), `session`, `timeout_ms` | JIT-compile `code` and evaluate `call`. Pre-includes `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<stdint.h>`, `<stdbool.h>`, and `host_*` state helpers. |
| `c_define` | `code` (req), `session` | Compile-check C definitions and append them to `session`. Rejected code leaves the session untouched. |
| `c_call` | `call` (req), `return_type`, `session`, `timeout_ms` | Invoke an expression or statement block against definitions already in `session`. |
| `c_reset` | `session` | Clear a session's C definitions and host-side key/value state. |
| `c_sessions` | *(none)* | List active sessions with definition byte counts and call counts. |

See [`examples/llm-mcp/README.md`](examples/llm-mcp/README.md) for architecture details and closed-loop JIT introspection examples.

---

## Original TinyCC Overview

Tiny C Compiler — C Scripting Everywhere — The Smallest ANSI C compiler.

### Features
- **SMALL!** Compile and execute C code everywhere, for example on rescue disks.
- **FAST!** `tcc` generates machine code for `i386`, `x86_64`, `arm`, `aarch64`, or `riscv64`. Compiles and links about 10 times faster than `gcc -O0`.
- **UNLIMITED!** Any C dynamic library can be used directly. TCC can compile itself.
- **SAFE!** Includes an optional memory and bound checker (`-b`).
- **C Scripting:** Add `#!/usr/local/bin/tcc -run` at the first line of your C source and execute it directly from the command line.

### Documentation & License
Please read `tcc-doc.texi` / `tcc-doc.pod` for full TCC documentation. TCC is distributed under the GNU Lesser General Public License (see [`COPYING`](COPYING)).
