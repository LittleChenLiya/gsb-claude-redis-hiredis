# Strict numeric mode (optional reader feature)

By default the hiredis reply parser keeps its classic, permissive numeric
behavior. An **optional strict numeric mode** can be enabled per reader to
validate `integer`, `double` and all length fields (bulk string lengths and
multi-bulk element counts) against a strict, locale-independent grammar.

## Enabling

```c
redisReader *r = redisReaderCreateWithFunctions(&fn);
redisReaderSetStrictNumeric(r, 1);   /* 1 = on, 0 = off (default) */
```

The flag lives in `redisReader.strict_numeric` and defaults to `0`, so
existing code is unaffected. It can be toggled at any time and applies to
all fields parsed afterwards, including fields that arrive split across
multiple `redisReaderFeed()` calls.

## Grammar enforced in strict mode

### Integers and length fields

* Optional leading `-`, then one or more ASCII decimal digits.
* Canonical form only: no leading zeros (`0` itself is fine), no `-0`,
  no `+` sign, no whitespace or any other byte before/after the digits.
* The **entire** field must be valid decimal; a partial `atoi()`-style
  parse (e.g. accepting the `12` of `12x`) is never accepted, no matter
  how the input is chunked across `redisReaderFeed()` calls.
* Values overflowing the `long long` range are rejected at parse time,
  **before** the value is used for any range check or allocation. In
  particular, an overflowing or extreme bulk/multi-bulk length can never
  trigger a `createString`/`createArray` allocation: the reader either
  rejects the length outright or waits for more data without allocating.
* `-1` remains the valid *null* length for `$` and `*` replies.

### Doubles

Only two forms are accepted:

1. The explicit literals `inf`, `-inf`, `nan`, `-nan` (case-insensitive).
2. A finite decimal number in canonical ASCII form:

   ```
   '-'? D+ ('.' D+)? (('e'|'E') ('+'|'-')? D+)?
   ```

   i.e. at least one integer digit, an optional fractional part with at
   least one digit, and an optional decimal exponent.

Everything else is rejected: hex floats (`0x1p3`), long forms such as
`infinity`, leading `+`, leading/trailing/hidden whitespace, and any
trailing garbage. The conversion never consults `LC_NUMERIC`, so `,1.5`
parses to exactly `1.5` under any locale (and `1,5` is rejected even in
locales whose decimal separator is a comma). This holds both for the
default ffc-based build and for builds compiled with
`HIREDIS_FLOAT_STRTOD` (strict mode then uses its own locale-independent
converter; note that in that fallback, values with more than 19
significant digits may be rounded slightly differently than `strtod`).

## Error reporting

Strict-mode protocol errors are reported as `REDIS_ERR_PROTOCOL` with a
message of the form:

```
Strict numeric: bad integer value at byte 1201
```

The byte offset is counted over the **cumulative input stream** — all
bytes ever passed to `redisReaderFeed()` — and stays correct even after
the reader trims its internal buffer. Offending fields cannot hide
behind chunk boundaries: validation always happens on the complete
field, so feeding `":1"` then `"2x\r\n"` is rejected exactly like
`":12x\r\n"` fed at once.

## Demo / acceptance program

`strict_numeric_demo.c` (repository root) is a single self-contained C
program that exercises the strict mode (chunked `12x` rejection, negative
integers, a comma-locale fixture for `1.5`, length-overflow allocation
blocking, null lengths, inf/nan literals, boundary chunking, cumulative
error offsets) and prints the parse result of every case:

```sh
gcc -std=c99 -Wall -Wextra -I. strict_numeric_demo.c read.c sds.c alloc.c -o strict_numeric_demo
./strict_numeric_demo
```

It exits with status 0 when all checks pass.
