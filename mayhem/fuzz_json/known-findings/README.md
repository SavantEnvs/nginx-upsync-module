# Known findings in the bundled cJSON (`src/ngx_http_json.c`)

`fuzz_json` reports 23 defects in a 5-minute run (`savantenvs/nginx-upsync-module/fuzz-json/1`). All are
real application bugs in the old Dave Gamble cJSON copy; the harness calls it the way
`ngx_http_upsync_*_parse_json` does (NUL-terminated reply body -> `cJSON_Parse` -> lookups ->
`cJSON_Print`). They collapse into **four root causes**:

| # | Root cause | First app frame(s) | Defects | Class (Mayhem) | Reproducer |
|---|------------|--------------------|---------|----------------|------------|
| 1 | `parse_string` length pass steps over the NUL after a trailing `\` | `parse_string:149` | 6 reads | OOB read (CWE-125) | `parse_string_trailing_backslash.json` (also `parse_string_fuzzer_find.json`) |
| 2 | `parse_string` `\u` handler consumes 4 (or 6) characters unchecked, walking past the NUL and desyncing from the length pass | `:155` reads, `:157` `:165` `:191` `:196` writes | 2 reads + 9 writes | OOB read / OOB write (CWE-125 / CWE-122) | `parse_string_short_u_escape.json` (read), `parse_string_u_escape_overrun_write.json` (write) |
| 3 | `print_object` sizes an empty object for `depth+3` bytes but, with `fmt` set, writes `depth+4` when `depth == 0` | `print_object:446` | 1 write | OOB write (CWE-122) | `print_object_empty_top_level.json` |
| 4 | `parse_number` casts an out-of-range `double` to `int` | `parse_number:114` | 5 | float-cast-overflow UB (CWE-197) | `parse_number_int_cast_overflow.json` |

The 23 split as 8 reads (6 at `:149`, 2 at `:155`), 9 `parse_string` writes (4 at `:157`, 2 at `:191`,
2 at `:196`, 1 at `:165`), 1 `print_object` write and 5 `parse_number` casts. Mayhem lists inputs per
defect only by backtrace, so the mapping to root causes is by first frame: every `parse_string` write
needs the unchecked `\u` skip of cause 2 (the other escapes copy at most one output byte per input pair,
exactly what the length pass counts), and the `:149` reads are cause 1 (or cause 2's overrun landing in the
length pass of the next string). Each reproducer was confirmed with the standalone build to fail at the
listed line. The reproducers are kept out of `testsuite/` on purpose: seeds are replayed every run and
would make every run fail on the first of them.

## 1. Trailing backslash skips the terminator (`parse_string:149`)

`while (*ptr!='\"' && *ptr && ++len) if (*ptr++ == '\\') ptr++;` — when the `\` is the last character
before the NUL, the second `ptr++` steps over the terminator and the loop keeps reading past the end of the
buffer (ASan: READ of size 1, immediately after the buffer). `"abc\` is enough. The original fuzzer find
(`parse_string_fuzzer_find.json`, `"\uut83d\ud` + two non-ASCII bytes + `\`) ends in this same trailing
backslash; an earlier version of this note attributed it to the `\u` handling.

**Fix (upstream).** After a backslash, only advance if `*ptr` is not NUL (in both passes).

## 2. Unchecked `\u` skip (`parse_string:155`, `:157`, `:165`, `:191`, `:196`)

For a `\u` escape the second pass does `sscanf(ptr+1,"%4x",&uc); ptr+=4;` without checking that four
characters remain, and without noticing that the length pass stopped early. Two consequences:

- **Read:** if the NUL falls inside those four characters (`"\u12`), `ptr` steps past it and the copy loop
  at `:155` reads beyond the buffer. `uc` is also left uninitialised when `sscanf` matches nothing. The
  surrogate-pair half (`ptr[1]`, `ptr[2]`, `ptr+3`, `ptr+=6`) has the same problem.
- **Write:** the length pass stops at the closing `"`, which may sit inside the four characters the `\u`
  handler skips (`"\u12"abcdefgh"`). The output buffer was sized for the 3 characters before the quote, but
  the copy loop resumes after the quote and keeps writing `bcdefgh` and the terminating NUL into it:
  heap-buffer-overflow WRITE at `:157` (or `:165`/`:191` for escapes and `:196` for the final NUL). Because
  the reply body is attacker-influenced, this is a controlled-length heap write, hence the higher severity
  (7.4) than the reads (6.4).

**Impact.** The module feeds every consul/etcd reply body to `cJSON_Parse`, so a malicious or compromised
registry can make the nginx worker read and write past a heap buffer.

**Fix (upstream).** After `case 'u':` verify that `ptr[1..4]` are all hex digits (stop on NUL or `"`)
before `sscanf`/`ptr+=4`, and require `ptr[1..6]` to be `\uXXXX` before consuming a surrogate pair's second
half. Later cJSON releases rewrote this function with explicit bounds checks.

## 3. Empty top-level object printed with formatting (`print_object:446`)

`cJSON_Print` on an empty object at depth 0 allocates `fmt ? depth+3 : 3` = 3 bytes, then writes `{`, `\n`,
`}` and the NUL: 4 bytes. `{}` as the whole document reproduces it. Nested empty objects are fine because
their depth is at least 1. The module itself calls `cJSON_Parse` only, so this is reachable through the
public API rather than through nginx.

**Fix (upstream).** Allocate `fmt ? depth+4 : 3`.

## 4. `double` to `int` cast of an out-of-range number (`parse_number:114`)

`item->valueint=(int)n;` is undefined behaviour when `n` does not fit in an `int` (`1e10`, `1e999` -> inf).
In practice it yields an arbitrary `valueint` and, under UBSan's `float-cast-overflow` check, aborts the
process. A registry reply with a large numeric field (a huge `weight`, say) triggers it before the module
reads the value.

**Fix (upstream).** Clamp: `valueint = n >= INT_MAX ? INT_MAX : n <= INT_MIN ? INT_MIN : (int)n;`
(what later cJSON releases do).

## Not filtered in the harness

The harness does not suppress any of these: no seed, size cap or input filter hides them, and the
findings stay visible in Mayhem until the bundled cJSON is fixed or replaced.
