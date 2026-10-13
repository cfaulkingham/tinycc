/*
 * tcc-mcp-server: an MCP stdio server giving an LLM a JIT-compiled C
 * "hand" via libtcc. Written in C and built with TCC itself:
 *
 *     ../../tcc -B../.. -I../.. -o tcc-mcp-server server.c ../../libtcc.c -lm
 *
 * Tools: c_eval, c_define, c_call, c_reset, c_sessions (see README.md).
 * Transport is newline-delimited JSON-RPC 2.0 (MCP stdio). The small
 * JSON reader/writer below only implements what MCP requests need.
 *
 * Each call recompiles (prelude + session definitions + code) into a
 * fresh TCCState and runs the generated __mcp_entry in a forked child,
 * so crashes and runaway loops in JIT code cannot take the server
 * down. C globals therefore reset per call; persistent values use the
 * host_* key/value functions, stored per session on the host side.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1 /* mkdtemp on macOS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "libtcc.h"

#define MAX_CODE_CHARS 256000
#define MAX_STDOUT_CHARS 65536
#define DEFAULT_TIMEOUT_MS 2000
#define MAX_TIMEOUT_MS 10000

static char g_libpath[PATH_MAX];
static char g_state_dir[PATH_MAX];

/* ---------- growable string ---------- */

typedef struct { char *buf; size_t len, cap; } Str;

static void str_grow(Str *s, size_t extra)
{
    if (s->len + extra + 1 > s->cap) {
        size_t cap = (s->len + extra + 1) * 2 + 64;
        s->buf = realloc(s->buf, cap);
        s->cap = cap;
    }
}

static void str_addn(Str *s, const char *p, size_t n)
{
    str_grow(s, n);
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = 0;
}

static void str_add(Str *s, const char *p) { str_addn(s, p, strlen(p)); }
static void str_addc(Str *s, char c) { str_addn(s, &c, 1); }

static void str_printf(Str *s, const char *fmt, ...)
{
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    str_add(s, tmp);
}

static const char *str_c(Str *s) { return s->buf ? s->buf : ""; }

static void str_free(Str *s) { free(s->buf); s->buf = NULL; s->len = s->cap = 0; }

/* ---------- JSON writing ---------- */

static void json_escape(Str *out, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    str_addc(out, '"');
    while (*p) {
        const unsigned char *start = p;
        while (*p >= 0x20 && *p != '"' && *p != '\\')
            p++;
        if (p > start)
            str_addn(out, (const char *)start, (size_t)(p - start));
        if (!*p)
            break;
        switch (*p++) {
        case '"': str_addn(out, "\\\"", 2); break;
        case '\\': str_addn(out, "\\\\", 2); break;
        case '\n': str_addn(out, "\\n", 2); break;
        case '\r': str_addn(out, "\\r", 2); break;
        case '\t': str_addn(out, "\\t", 2); break;
        case '\b': str_addn(out, "\\b", 2); break;
        case '\f': str_addn(out, "\\f", 2); break;
        default:
            str_printf(out, "\\u%04x", p[-1]);
            break;
        }
    }
    str_addc(out, '"');
}

/* ---------- minimal JSON reading ----------
 * Not a DOM: we locate values by key inside an object and decode the
 * scalars we need. Strings are fully unescaped, including \uXXXX. */

static const char *json_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p must point at the opening quote; returns past the closing quote. */
static const char *json_skip_string(const char *p)
{
    for (p++; *p && *p != '"'; p++)
        if (*p == '\\' && p[1]) p++;
    return *p == '"' ? p + 1 : p;
}

static const char *json_skip_value(const char *p)
{
    p = json_ws(p);
    if (*p == '"') return json_skip_string(p);
    if (*p == '{' || *p == '[') {
        char open = *p, close = open == '{' ? '}' : ']';
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') { p = json_skip_string(p) - 1; continue; }
            if (*p == open) depth++;
            else if (*p == close && --depth == 0) return p + 1;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p;
}

static void utf8_encode(Str *out, unsigned cp)
{
    if (cp < 0x80) str_addc(out, (char)cp);
    else if (cp < 0x800) {
        str_addc(out, (char)(0xc0 | (cp >> 6)));
        str_addc(out, (char)(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        str_addc(out, (char)(0xe0 | (cp >> 12)));
        str_addc(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
        str_addc(out, (char)(0x80 | (cp & 0x3f)));
    } else {
        str_addc(out, (char)(0xf0 | (cp >> 18)));
        str_addc(out, (char)(0x80 | ((cp >> 12) & 0x3f)));
        str_addc(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
        str_addc(out, (char)(0x80 | (cp & 0x3f)));
    }
}

/* Decode the JSON string at p (opening quote) into a malloc'd string. */
static char *json_decode_string(const char *p)
{
    Str out = {0};
    p++;
    while (*p && *p != '"') {
        const char *start = p;
        while (*p && *p != '"' && *p != '\\')
            p++;
        if (p > start)
            str_addn(&out, start, (size_t)(p - start));
        if (*p != '\\')
            break;
        p++;
        if (!*p)
            break;
        switch (*p++) {
        case 'n': str_addc(&out, '\n'); break;
        case 't': str_addc(&out, '\t'); break;
        case 'r': str_addc(&out, '\r'); break;
        case 'b': str_addc(&out, '\b'); break;
        case 'f': str_addc(&out, '\f'); break;
        case 'u': {
            unsigned cp = (unsigned)strtoul(p, NULL, 16);
            p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && p[0] == '\\' && p[1] == 'u') {
                unsigned lo = (unsigned)strtoul(p + 2, NULL, 16);
                if (lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    p += 6;
                }
            }
            utf8_encode(&out, cp);
            break;
        }
        default: str_addc(&out, p[-1]); break; /* \" \\ \/ */
        }
    }
    if (!out.buf) return strdup("");
    return out.buf;
}

/* Find "key" at the top level of the object at p; return its value. */
static const char *json_find(const char *p, const char *key)
{
    p = json_ws(p);
    if (*p != '{') return NULL;
    for (p++; ; p++) {
        p = json_ws(p);
        if (*p != '"') return NULL;
        {
            char *k = json_decode_string(p);
            int match = strcmp(k, key) == 0;
            free(k);
            p = json_skip_string(p);
            p = json_ws(p);
            if (*p != ':') return NULL;
            p = json_ws(p + 1);
            if (match) return p;
            p = json_skip_value(p);
            p = json_ws(p);
            if (*p != ',') return NULL;
        }
    }
}

static char *json_get_string(const char *obj, const char *key, const char *def_value)
{
    const char *v = json_find(obj, key);
    if (!v || *v != '"') return strdup(def_value);
    return json_decode_string(v);
}

static long json_get_int(const char *obj, const char *key, long def_value)
{
    const char *v = json_find(obj, key);
    if (!v) return def_value;
    return strtol(v, NULL, 10);
}

/* Copy the raw JSON span of a value (used to echo request ids). */
static char *json_raw_span(const char *value_start)
{
    const char *end = json_skip_value(value_start);
    size_t n = (size_t)(end - value_start);
    char *out = malloc(n + 1);
    memcpy(out, value_start, n);
    out[n] = 0;
    return out;
}

/* ---------- host key/value state (exposed to JIT code) ---------- */

#define KV_MAX 256
#define KV_KEY_MAX 64

typedef struct {
    char key[KV_KEY_MAX];
    char type; /* 'i', 'd' or 's' */
    long long ival;
    double dval;
    char *sval;
} KvEntry;

static KvEntry kv[KV_MAX];
static int kv_count;

static KvEntry *kv_find(const char *key, char type, int create)
{
    int i;
    for (i = 0; i < kv_count; i++)
        if (kv[i].type == type && strcmp(kv[i].key, key) == 0)
            return &kv[i];
    if (!create || kv_count >= KV_MAX) return NULL;
    {
        KvEntry *e = &kv[kv_count++];
        snprintf(e->key, sizeof e->key, "%s", key);
        e->type = type;
        e->ival = 0; e->dval = 0; e->sval = NULL;
        return e;
    }
}

long long host_get_int(const char *key)
{
    KvEntry *e = kv_find(key, 'i', 0);
    return e ? e->ival : 0;
}

void host_set_int(const char *key, long long v)
{
    KvEntry *e = kv_find(key, 'i', 1);
    if (e) e->ival = v;
}

double host_get_double(const char *key)
{
    KvEntry *e = kv_find(key, 'd', 0);
    return e ? e->dval : 0.0;
}

void host_set_double(const char *key, double v)
{
    KvEntry *e = kv_find(key, 'd', 1);
    if (e) e->dval = v;
}

const char *host_get_str(const char *key)
{
    KvEntry *e = kv_find(key, 's', 0);
    return (e && e->sval) ? e->sval : "";
}

void host_set_str(const char *key, const char *v)
{
    KvEntry *e = kv_find(key, 's', 1);
    if (e) { free(e->sval); e->sval = strdup(v ? v : ""); }
}

int host_has(const char *key)
{
    return kv_find(key, 'i', 0) || kv_find(key, 'd', 0) || kv_find(key, 's', 0);
}

long long host_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* State file: one `key \t type \t value` line per entry; string
 * values escape \\, \t, \n and \r. */

static void kv_escape_file(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '\\': fputs("\\\\", f); break;
        case '\t': fputs("\\t", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        default: fputc(*s, f);
        }
    }
}

static char *kv_unescape(const char *s)
{
    char *out = malloc(strlen(s) + 1), *p = out;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *p++ = (*s == 't') ? '\t' : (*s == 'n') ? '\n' : (*s == 'r') ? '\r' : *s;
        } else
            *p++ = *s;
    }
    *p = 0;
    return out;
}

static void kv_load(const char *path)
{
    FILE *f;
    char line[8192];
    kv_count = 0;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t'), *t2, *nl;
        char type;
        KvEntry *e;
        if (!t1) continue;
        t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t1 = *t2 = 0;
        nl = strchr(t2 + 1, '\n');
        if (nl) *nl = 0;
        type = t1[1];
        e = kv_find(line, type, 1);
        if (!e) continue;
        if (type == 'i') e->ival = strtoll(t2 + 1, NULL, 10);
        else if (type == 'd') e->dval = strtod(t2 + 1, NULL);
        else if (type == 's') { free(e->sval); e->sval = kv_unescape(t2 + 1); }
    }
    fclose(f);
}

static void kv_save(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return;
    for (i = 0; i < kv_count; i++) {
        KvEntry *e = &kv[i];
        if (strpbrk(e->key, "\t\n")) continue;
        fprintf(f, "%s\t%c\t", e->key, e->type);
        if (e->type == 'i') fprintf(f, "%lld", e->ival);
        else if (e->type == 'd') fprintf(f, "%.17g", e->dval);
        else if (e->type == 's') kv_escape_file(f, e->sval ? e->sval : "");
        fputc('\n', f);
    }
    fclose(f);
}

/* ---------- sessions ---------- */

#define MAX_SESSIONS 64

typedef struct {
    char name[65];
    Str defs;
    long calls;
} Session;

static Session sessions[MAX_SESSIONS];
static int session_count;

static int session_name_ok(const char *name)
{
    size_t i, n = strlen(name);
    if (n == 0 || n > 64) return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-')
            return 0;
    return 1;
}

static Session *session_get(const char *name)
{
    int i;
    Session *s;
    for (i = 0; i < session_count; i++)
        if (strcmp(sessions[i].name, name) == 0) return &sessions[i];
    if (session_count >= MAX_SESSIONS) return NULL;
    s = &sessions[session_count++];
    snprintf(s->name, sizeof s->name, "%s", name);
    s->defs.buf = NULL; s->defs.len = s->defs.cap = 0;
    s->calls = 0;
    return s;
}

static void session_state_path(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s.state", g_state_dir, name);
}

static void session_reset(const char *name)
{
    int i;
    char path[PATH_MAX];
    for (i = 0; i < session_count; i++) {
        if (strcmp(sessions[i].name, name) == 0) {
            str_free(&sessions[i].defs);
            sessions[i] = sessions[--session_count];
            break;
        }
    }
    session_state_path(name, path, sizeof path);
    unlink(path);
}

/* ---------- compile + run engine ---------- */

static const char PRELUDE[] =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "#include <math.h>\n"
    "#include <stdint.h>\n"
    "#include <stdbool.h>\n"
    "extern long long host_get_int(const char *key);\n"
    "extern void host_set_int(const char *key, long long v);\n"
    "extern double host_get_double(const char *key);\n"
    "extern void host_set_double(const char *key, double v);\n"
    "extern const char *host_get_str(const char *key);\n"
    "extern void host_set_str(const char *key, const char *v);\n"
    "extern int host_has(const char *key);\n"
    "extern long long host_now_ms(void);\n";

typedef struct {
    int ok;
    const char *status;   /* ok, compile_error, crashed, timed_out, ... */
    Str diagnostics;
    Str stdout_text;
    int stdout_truncated;
    char value[8192];     /* raw value text from the child (escaped if string) */
    int value_is_null;
    int signal_no;
    int exit_code;
    Str symbols;          /* check mode: one symbol per line */
    double elapsed_ms;
} RunResult;

static Str g_err;
static Str g_symbols;

static void on_tcc_error(void *opaque, const char *msg)
{
    (void)opaque;
    str_add(&g_err, msg);
    str_addc(&g_err, '\n');
}

static void on_symbol(void *ctx, const char *name, const void *val)
{
    (void)ctx; (void)val;
    if (name[0]) { str_add(&g_symbols, name); str_addc(&g_symbols, '\n'); }
}

typedef long long (*EntryInt)(void);
typedef double (*EntryDouble)(void);
typedef const char *(*EntryStr)(void);
typedef void (*EntryVoid)(void);

/* In the forked child; stdout/stderr already point at the pipe. */
static void child_run(void *entry, const char *ret,
                      const char *result_path, const char *state_out,
                      int cpu_sec)
{
    struct rlimit rl;
    FILE *rf;

    rl.rlim_cur = rl.rlim_max = (rlim_t)(cpu_sec > 0 ? cpu_sec : 1);
    setrlimit(RLIMIT_CPU, &rl);
    rl.rlim_cur = rl.rlim_max = 0;
    setrlimit(RLIMIT_CORE, &rl);

    rf = fopen(result_path, "w");
    if (strcmp(ret, "double") == 0) {
        double v = ((EntryDouble)entry)();
        if (rf) fprintf(rf, "value=%.17g\n", v);
    } else if (strcmp(ret, "string") == 0) {
        const char *v = ((EntryStr)entry)();
        if (rf) {
            fputs("value=", rf);
            if (v) kv_escape_file(rf, v);
            fputc('\n', rf);
            fprintf(rf, "value_is_null=%d\n", v == NULL);
        }
    } else if (strcmp(ret, "void") == 0) {
        ((EntryVoid)entry)();
    } else { /* int / long: the generated wrapper returns long long */
        long long v = ((EntryInt)entry)();
        if (rf) fprintf(rf, "value=%lld\n", v);
    }
    if (rf) fclose(rf);
    kv_save(state_out);
    fflush(NULL);
    _exit(0);
}

static void result_parse_file(RunResult *r, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[8192];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, "value_is_null=", 14) == 0)
            r->value_is_null = line[14] == '1';
        else if (strncmp(line, "value=", 6) == 0)
            snprintf(r->value, sizeof r->value, "%s", line + 6);
    }
    fclose(f);
}

static void run_source(RunResult *r, const char *source, const char *ret,
                       const char *session_name, long timeout_ms)
{
    TCCState *s;
    struct timespec t0, t1;
    int is_check = strcmp(ret, "check") == 0;
    char state_in[PATH_MAX], dir[PATH_MAX], result_path[PATH_MAX], state_out[PATH_MAX];
    const char *tmpdir = getenv("TMPDIR");

    memset(r, 0, sizeof *r);
    r->status = "worker_error";
    r->exit_code = -1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (timeout_ms < 50) timeout_ms = 50;
    if (timeout_ms > MAX_TIMEOUT_MS) timeout_ms = MAX_TIMEOUT_MS;

    session_state_path(session_name, state_in, sizeof state_in);
    snprintf(dir, sizeof dir, "%s/tcc-mcp-call-XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    if (!mkdtemp(dir)) goto done_no_timing;
    snprintf(result_path, sizeof result_path, "%s/result.txt", dir);
    snprintf(state_out, sizeof state_out, "%s/state.txt", dir);
    kv_load(state_in);

    str_free(&g_err);
    str_free(&g_symbols);
    s = tcc_new();
    if (!s) goto cleanup_files;
    tcc_set_error_func(s, NULL, on_tcc_error);
    tcc_set_lib_path(s, g_libpath);
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_add_symbol(s, "host_get_int", (const void *)host_get_int);
    tcc_add_symbol(s, "host_set_int", (const void *)host_set_int);
    tcc_add_symbol(s, "host_get_double", (const void *)host_get_double);
    tcc_add_symbol(s, "host_set_double", (const void *)host_set_double);
    tcc_add_symbol(s, "host_get_str", (const void *)host_get_str);
    tcc_add_symbol(s, "host_set_str", (const void *)host_set_str);
    tcc_add_symbol(s, "host_has", (const void *)host_has);
    tcc_add_symbol(s, "host_now_ms", (const void *)host_now_ms);

    if (tcc_compile_string(s, source) == -1) {
        r->status = "compile_error";
        goto cleanup_state;
    }
    if (tcc_relocate(s) < 0) {
        r->status = "relocate_error";
        goto cleanup_state;
    }
    if (is_check) {
        tcc_list_symbols(s, NULL, on_symbol);
        r->symbols = g_symbols;
        g_symbols.buf = NULL; g_symbols.len = g_symbols.cap = 0;
        r->ok = 1;
        r->status = "ok";
        goto cleanup_state;
    }
    {
        void *entry = tcc_get_symbol(s, "__mcp_entry");
        int pfd[2], status = 0, timed_out = 0;
        int cpu_sec = (int)((timeout_ms + 999) / 1000);
        pid_t pid;
        if (!entry) { r->status = "no_entry"; goto cleanup_state; }
        if (pipe(pfd) != 0) goto cleanup_state;
        fflush(NULL);
        pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDONLY);
            setpgid(0, 0);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
            close(pfd[0]);
            dup2(pfd[1], STDOUT_FILENO);
            dup2(pfd[1], STDERR_FILENO);
            close(pfd[1]);
            child_run(entry, ret, result_path, state_out, cpu_sec);
            _exit(70);
        }
        setpgid(pid, pid); /* race-free enough alongside the child's call */
        close(pfd[1]);
        { /* read child output until EOF or the wall-clock deadline */
            long long deadline = host_now_ms() + timeout_ms;
            size_t kept = 0;
            int flags = fcntl(pfd[0], F_GETFL, 0);
            fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
            for (;;) {
                struct pollfd p = { pfd[0], POLLIN, 0 };
                long long remain = deadline - host_now_ms();
                int pr;
                char buf[8192];
                ssize_t n;
                if (remain <= 0) { timed_out = 1; killpg(pid, SIGKILL); break; }
                pr = poll(&p, 1, remain > 100 ? 100 : (int)remain);
                if (pr > 0) {
                    n = read(pfd[0], buf, sizeof buf);
                    if (n > 0) {
                        size_t room = kept < MAX_STDOUT_CHARS ? MAX_STDOUT_CHARS - kept : 0;
                        size_t w = (size_t)n < room ? (size_t)n : room;
                        if (w) str_addn(&r->stdout_text, buf, w);
                        if (w < (size_t)n) r->stdout_truncated = 1;
                        kept += (size_t)n;
                        continue;
                    }
                    if (n == 0 || (errno != EAGAIN && errno != EINTR)) break;
                }
            }
        }
        close(pfd[0]);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
        result_parse_file(r, result_path);
        if (timed_out) {
            r->status = "timed_out";
        } else if (WIFSIGNALED(status)) {
            r->status = "crashed";
            r->signal_no = WTERMSIG(status);
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            r->status = "exited";
            r->exit_code = WEXITSTATUS(status);
        } else {
            rename(state_out, state_in); /* commit host state on clean exit */
            r->ok = 1;
            r->status = "ok";
        }
    }

cleanup_state:
    tcc_delete(s);
cleanup_files:
    r->diagnostics = g_err;
    g_err.buf = NULL; g_err.len = g_err.cap = 0;
    unlink(result_path);
    unlink(state_out);
    rmdir(dir);
done_no_timing:
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r->elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                    (t1.tv_nsec - t0.tv_nsec) / 1e6;
}

/* Generate __mcp_entry so TCC typechecks/marshals the call for us. */
static void build_source(Str *out, const char *defs, const char *code,
                         const char *call, const char *return_type)
{
    str_add(out, PRELUDE);
    str_add(out, "#line 1 \"session.c\"\n");
    str_add(out, defs);
    str_add(out, "\n#line 1 \"eval.c\"\n");
    str_add(out, code);
    str_add(out, "\n#line 1 \"call.c\"\n");
    if (!call) return;
    if (strcmp(return_type, "void") == 0) {
        str_add(out, "void __mcp_entry(void) {\n");
        str_add(out, call);
        str_add(out, "\n}\n");
    } else if (strcmp(return_type, "double") == 0) {
        str_add(out, "double __mcp_entry(void) { return (double)(");
        str_add(out, call);
        str_add(out, "); }\n");
    } else if (strcmp(return_type, "string") == 0) {
        str_add(out, "const char *__mcp_entry(void) { return (const char *)(");
        str_add(out, call);
        str_add(out, "); }\n");
    } else {
        str_add(out, "long long __mcp_entry(void) { return (long long)(");
        str_add(out, call);
        str_add(out, "); }\n");
    }
}

static const char *signal_name(int no, char *buf, size_t size)
{
    switch (no) {
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGKILL: return "SIGKILL";
    case SIGSEGV: return "SIGSEGV";
    case SIGSYS: return "SIGSYS";
    case SIGTRAP: return "SIGTRAP";
    case SIGXCPU: return "SIGXCPU";
    case SIGXFSZ: return "SIGXFSZ";
    default: snprintf(buf, size, "SIG%d", no); return buf;
    }
}

/* Append the JSON payload fields shared by all run-based tools. */
static void payload_from_run(Str *out, RunResult *r, const char *return_type)
{
    char sigbuf[32];
    str_printf(out, "\"ok\":%s,\"status\":\"%s\",\"elapsed_ms\":%.2f,\"stdout\":",
               r->ok ? "true" : "false", r->status, r->elapsed_ms);
    json_escape(out, str_c(&r->stdout_text));
    if (r->stdout_truncated) str_add(out, ",\"stdout_truncated\":true");
    if (r->diagnostics.len) {
        str_add(out, ",\"diagnostics\":");
        json_escape(out, str_c(&r->diagnostics));
    }
    if (r->signal_no) {
        str_add(out, ",\"signal\":");
        json_escape(out, signal_name(r->signal_no, sigbuf, sizeof sigbuf));
    }
    if (strcmp(r->status, "exited") == 0)
        str_printf(out, ",\"exit_code\":%d", r->exit_code);
    if (r->ok && return_type) {
        str_add(out, ",\"result\":");
        if (strcmp(return_type, "void") == 0 ||
            (strcmp(return_type, "string") == 0 && r->value_is_null)) {
            str_add(out, "null");
        } else if (strcmp(return_type, "string") == 0) {
            char *plain = kv_unescape(r->value);
            json_escape(out, plain);
            free(plain);
        } else if (strcmp(return_type, "double") == 0) {
            if (strstr(r->value, "inf") || strstr(r->value, "nan"))
                json_escape(out, r->value); /* non-finite: not valid JSON numbers */
            else
                str_add(out, r->value[0] ? r->value : "0");
        } else if (strcmp(return_type, "long") == 0) {
            str_printf(out, "%lld", strtoll(r->value, NULL, 10));
        } else {
            str_printf(out, "%d", (int)strtoll(r->value, NULL, 10));
        }
    }
}

static void payload_free_run(RunResult *r)
{
    str_free(&r->diagnostics);
    str_free(&r->stdout_text);
    str_free(&r->symbols);
}

static void bad_args(Str *out, const char *msg, int *ok)
{
    *ok = 0;
    str_add(out, "{\"ok\":false,\"status\":\"bad_arguments\",\"error\":");
    json_escape(out, msg);
    str_addc(out, '}');
}

/* ---------- tools ---------- */

static int return_type_ok(const char *rt)
{
    return strcmp(rt, "int") == 0 || strcmp(rt, "long") == 0 ||
           strcmp(rt, "double") == 0 || strcmp(rt, "string") == 0 ||
           strcmp(rt, "void") == 0;
}

static void tool_eval(Str *out, const char *args, int with_code, int *ok)
{
    char *code = NULL, *call = NULL, *rt = NULL, *sname = NULL;
    Session *sess;
    Str source = {0};
    RunResult r;

    call = json_get_string(args, "call", "");
    if (!call[0]) { bad_args(out, "missing required string: call", ok); goto done; }
    rt = json_get_string(args, "return_type", "int");
    if (!return_type_ok(rt)) {
        bad_args(out, "return_type must be int, long, double, string or void", ok);
        goto done;
    }
    code = with_code ? json_get_string(args, "code", "") : strdup("");
    if (strlen(code) > MAX_CODE_CHARS) {
        bad_args(out, "code exceeds the size limit", ok);
        goto done;
    }
    sname = json_get_string(args, "session", "default");
    if (!session_name_ok(sname)) {
        bad_args(out, "invalid session name (want [A-Za-z0-9_-], 1-64 chars)", ok);
        goto done;
    }
    sess = session_get(sname);
    if (!sess) { bad_args(out, "too many sessions", ok); goto done; }
    sess->calls++;

    build_source(&source, str_c(&sess->defs), code, call, rt);
    run_source(&r, str_c(&source), rt, sname,
               json_get_int(args, "timeout_ms", DEFAULT_TIMEOUT_MS));
    *ok = r.ok;
    str_addc(out, '{');
    payload_from_run(out, &r, rt);
    str_add(out, ",\"session\":");
    json_escape(out, sname);
    str_add(out, ",\"return_type\":");
    json_escape(out, rt);
    str_addc(out, '}');
    payload_free_run(&r);
done:
    str_free(&source);
    free(code); free(call); free(rt); free(sname);
}

static void symbols_json(Str *out, const Str *symbols)
{
    const char *p = str_c((Str *)symbols);
    int first = 1;
    str_addc(out, '[');
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n) {
            char name[512];
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, p, n);
            name[n] = 0;
            if (!first) str_addc(out, ',');
            first = 0;
            json_escape(out, name);
        }
        p = nl ? nl + 1 : p + n;
    }
    str_addc(out, ']');
}

static void tool_define(Str *out, const char *args, int *ok)
{
    char *code = NULL, *sname = NULL;
    Session *sess;
    Str candidate = {0}, source = {0};
    RunResult r;

    code = json_get_string(args, "code", "");
    if (!code[0]) { bad_args(out, "missing required string: code", ok); goto done; }
    if (strlen(code) > MAX_CODE_CHARS) {
        bad_args(out, "code exceeds the size limit", ok);
        goto done;
    }
    sname = json_get_string(args, "session", "default");
    if (!session_name_ok(sname)) {
        bad_args(out, "invalid session name (want [A-Za-z0-9_-], 1-64 chars)", ok);
        goto done;
    }
    sess = session_get(sname);
    if (!sess) { bad_args(out, "too many sessions", ok); goto done; }

    str_add(&candidate, str_c(&sess->defs));
    if (candidate.len) str_addc(&candidate, '\n');
    str_add(&candidate, code);
    build_source(&source, "", str_c(&candidate), NULL, "int");
    run_source(&r, str_c(&source), "check", sname, DEFAULT_TIMEOUT_MS);
    *ok = r.ok;
    str_addc(out, '{');
    payload_from_run(out, &r, NULL);
    str_add(out, ",\"session\":");
    json_escape(out, sname);
    if (r.ok) {
        str_free(&sess->defs);
        sess->defs = candidate;
        candidate.buf = NULL; candidate.len = candidate.cap = 0;
        str_add(out, ",\"symbols\":");
        symbols_json(out, &r.symbols);
        str_printf(out, ",\"definitions_chars\":%zu", sess->defs.len);
    } else {
        str_add(out, ",\"note\":\"definitions rejected; the session was left unchanged\"");
    }
    str_addc(out, '}');
    payload_free_run(&r);
done:
    str_free(&candidate);
    str_free(&source);
    free(code); free(sname);
}

static void tool_reset(Str *out, const char *args, int *ok)
{
    char *sname = json_get_string(args, "session", "default");
    if (!session_name_ok(sname)) {
        bad_args(out, "invalid session name (want [A-Za-z0-9_-], 1-64 chars)", ok);
        free(sname);
        return;
    }
    session_reset(sname);
    *ok = 1;
    str_add(out, "{\"ok\":true,\"status\":\"ok\",\"reset\":true,\"session\":");
    json_escape(out, sname);
    str_addc(out, '}');
    free(sname);
}

static void tool_sessions(Str *out, int *ok)
{
    int i;
    *ok = 1;
    str_add(out, "{\"ok\":true,\"status\":\"ok\",\"sessions\":[");
    for (i = 0; i < session_count; i++) {
        if (i) str_addc(out, ',');
        str_add(out, "{\"name\":");
        json_escape(out, sessions[i].name);
        str_printf(out, ",\"definitions_chars\":%zu,\"calls\":%ld}",
                   sessions[i].defs.len, sessions[i].calls);
    }
    str_add(out, "]}");
}

static const char TOOL_DEFS_JSON[] =
    "[{\"name\":\"c_eval\",\"description\":\"JIT-compile C code with TinyCC and evaluate "
    "one typed call. `code` holds definitions; `call` is an expression such as "
    "`fib(10)` whose value is returned as `return_type` - except for `void`, where "
    "`call` is a block of statements and only stdout is returned. stdio/stdlib/"
    "string/math/stdint/stdbool are pre-included. Session definitions (c_define) "
    "are visible. C globals reset on every call; persist values with "
    "host_set_int/host_get_int, host_set_double/host_get_double, "
    "host_set_str/host_get_str. Crashes and timeouts are reported, never fatal.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"code\":{\"type\":\"string\",\"description\":\"C definitions to compile (may be empty)\"},"
    "\"call\":{\"type\":\"string\",\"description\":\"Expression to evaluate, or statements for void\"},"
    "\"return_type\":{\"type\":\"string\",\"enum\":[\"int\",\"long\",\"double\",\"string\",\"void\"],\"default\":\"int\"},"
    "\"session\":{\"type\":\"string\",\"default\":\"default\"},"
    "\"timeout_ms\":{\"type\":\"integer\",\"default\":2000,\"maximum\":10000}},"
    "\"required\":[\"call\"]}},"
    "{\"name\":\"c_define\",\"description\":\"Validate C definitions by compiling them, "
    "then append them to a session so later c_eval/c_call invocations can use them. "
    "On a compile error the diagnostics are returned and the session is unchanged.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"code\":{\"type\":\"string\"},\"session\":{\"type\":\"string\",\"default\":\"default\"}},"
    "\"required\":[\"code\"]}},"
    "{\"name\":\"c_call\",\"description\":\"Like c_eval with empty `code`: call something "
    "already defined in the session.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"call\":{\"type\":\"string\"},"
    "\"return_type\":{\"type\":\"string\",\"enum\":[\"int\",\"long\",\"double\",\"string\",\"void\"],\"default\":\"int\"},"
    "\"session\":{\"type\":\"string\",\"default\":\"default\"},"
    "\"timeout_ms\":{\"type\":\"integer\",\"default\":2000,\"maximum\":10000}},"
    "\"required\":[\"call\"]}},"
    "{\"name\":\"c_reset\",\"description\":\"Clear a session's definitions and host-side state.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"session\":{\"type\":\"string\",\"default\":\"default\"}}}},"
    "{\"name\":\"c_sessions\",\"description\":\"List sessions with their definition "
    "sizes and call counts.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]";

/* ---------- MCP / JSON-RPC dispatch ---------- */

/* Returns 1 if a response was written to out, 0 for notifications. */
static int handle_request(const char *line, Str *out)
{
    const char *id_value = json_find(line, "id");
    const char *params = json_find(line, "params");
    char *method, *id_raw;

    if (!id_value) return 0; /* notification: no reply */
    id_raw = json_raw_span(id_value);
    method = json_get_string(line, "method", "");
    if (!params) params = "{}";

    if (strcmp(method, "initialize") == 0) {
        char *version = json_get_string(params, "protocolVersion", "2025-03-26");
        str_printf(out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{", id_raw);
        str_add(out, "\"protocolVersion\":");
        json_escape(out, version);
        str_add(out, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
                     "\"serverInfo\":{\"name\":\"tcc-mcp\",\"version\":\"0.2.0\"},"
                     "\"instructions\":\"Use c_define to build up C definitions in a "
                     "session, then c_call to invoke them with a typed return. "
                     "c_eval does both in one shot.\"}}");
        free(version);
    } else if (strcmp(method, "ping") == 0) {
        str_printf(out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{}}", id_raw);
    } else if (strcmp(method, "tools/list") == 0) {
        str_printf(out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":", id_raw);
        str_add(out, TOOL_DEFS_JSON);
        str_add(out, "}}");
    } else if (strcmp(method, "tools/call") == 0) {
        char *name = json_get_string(params, "name", "");
        const char *args = json_find(params, "arguments");
        Str payload = {0};
        int ok = 0;
        if (!args) args = "{}";
        if (strcmp(name, "c_eval") == 0) tool_eval(&payload, args, 1, &ok);
        else if (strcmp(name, "c_call") == 0) tool_eval(&payload, args, 0, &ok);
        else if (strcmp(name, "c_define") == 0) tool_define(&payload, args, &ok);
        else if (strcmp(name, "c_reset") == 0) tool_reset(&payload, args, &ok);
        else if (strcmp(name, "c_sessions") == 0) tool_sessions(&payload, &ok);
        else str_printf(&payload, "{\"ok\":false,\"status\":\"unknown_tool\"}");
        str_printf(out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":["
                        "{\"type\":\"text\",\"text\":", id_raw);
        json_escape(out, str_c(&payload));
        str_printf(out, "}],\"isError\":%s}}", ok ? "false" : "true");
        str_free(&payload);
        free(name);
    } else {
        str_printf(out, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,"
                        "\"message\":\"method not found: %s\"}}", id_raw, method);
    }
    free(method);
    free(id_raw);
    return 1;
}

static void init_paths(const char *argv0)
{
    const char *env = getenv("TCC_MCP_LIBPATH");
    if (env && *env) {
        snprintf(g_libpath, sizeof g_libpath, "%s", env);
    } else {
        const char *slash = strrchr(argv0, '/');
        if (slash)
            snprintf(g_libpath, sizeof g_libpath, "%.*s/../..",
                     (int)(slash - argv0), argv0);
        else
            snprintf(g_libpath, sizeof g_libpath, "../..");
    }
    env = getenv("TCC_MCP_STATE_DIR");
    if (env && *env) {
        snprintf(g_state_dir, sizeof g_state_dir, "%s", env);
        mkdir(g_state_dir, 0700);
    } else {
        const char *tmpdir = getenv("TMPDIR");
        snprintf(g_state_dir, sizeof g_state_dir, "%s/tcc-mcp-XXXXXX",
                 tmpdir && *tmpdir ? tmpdir : "/tmp");
        if (!mkdtemp(g_state_dir))
            snprintf(g_state_dir, sizeof g_state_dir, "/tmp");
    }
}

int main(int argc, char **argv)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    (void)argc;
    /* Some async clients (Grok's tokio child process) leak O_NONBLOCK
     * into the spawned child's stdin/stdout; a blocking getline server
     * must clear it or the first read fails with EAGAIN and we exit. */
    {
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl >= 0) fcntl(STDIN_FILENO, F_SETFL, fl & ~O_NONBLOCK);
        fl = fcntl(STDOUT_FILENO, F_GETFL, 0);
        if (fl >= 0) fcntl(STDOUT_FILENO, F_SETFL, fl & ~O_NONBLOCK);
    }
    init_paths(argv[0]);
    fprintf(stderr, "[tcc-mcp] libpath=%s state_dir=%s\n", g_libpath, g_state_dir);
    {
        char probe[PATH_MAX];
        snprintf(probe, sizeof probe, "%s/libtcc1.a", g_libpath);
        if (access(probe, F_OK) != 0)
            fprintf(stderr, "[tcc-mcp] warning: %s not found; set TCC_MCP_LIBPATH "
                            "to the tinycc build tree\n", probe);
    }
    /* Clients disagree on stdio framing: MCP specifies newline-delimited
     * JSON, but some (e.g. Grok) use LSP-style Content-Length headers.
     * Auto-detect per message and reply in the same framing. */
    while ((n = getline(&line, &cap, stdin)) >= 0) {
        Str out = {0};
        char *body = NULL;
        const char *request = NULL;
        int framed = 0;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (n == 0) continue;
        if (line[0] == '{') {
            request = line; /* newline-delimited JSON */
        } else {
            /* Header block: read until the blank line, then the body. */
            long length = -1;
            for (;;) {
                if (strncasecmp(line, "Content-Length:", 15) == 0)
                    length = strtol(line + 15, NULL, 10);
                n = getline(&line, &cap, stdin);
                if (n < 0) break;
                while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                    line[--n] = 0;
                if (n == 0) break;
            }
            if (n < 0 || length < 0) { str_free(&out); break; }
            body = malloc((size_t)length + 1);
            if (fread(body, 1, (size_t)length, stdin) != (size_t)length) {
                free(body);
                str_free(&out);
                break;
            }
            body[length] = 0;
            request = body;
            framed = 1;
        }
        if (handle_request(request, &out)) {
            if (framed)
                fprintf(stdout, "Content-Length: %zu\r\n\r\n", out.len);
            fwrite(str_c(&out), 1, out.len, stdout);
            if (!framed) fputc('\n', stdout);
            fflush(stdout);
        }
        free(body);
        str_free(&out);
    }
    free(line);
    return 0;
}
