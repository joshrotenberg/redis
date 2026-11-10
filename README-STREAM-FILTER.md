# Consumer group filters (prototype)

This branch, `experiment/stream-group-filters`, adds filters to stream
consumer groups. It is a prototype on Redis `unstable` at `28c3a1820`, not a
proposed change yet. This file explains it while the branch lives; it is not
meant to be merged.

## What it does

A consumer group can carry a filter expression over entry fields.

- `XREADGROUP ... >` on a filtered group delivers only the entries the filter
  matches.
- The group's cursor (`last-delivered-id`, `entries-read`) moves past every
  entry the read examines, matching or not. The pending entries list holds
  deliveries only.
- An `ELSE` group receives the entries that no other filtered group on the
  stream matches. One per stream.
- `MAXSCAN` caps the entries one read examines. When the cap is reached
  before a match, the reply is an empty entry list, and the client reads
  again.
- A blocked `XREADGROUP` on a filtered group wakes only for an entry its
  group takes, or when the entries after its cursor exceed its scan cap.

Groups without a filter behave as before.

## Example

Run on `80c83d58b`. Replies abridged.

```text
> XADD ticks 1-1 type fx sym EURUSD px 1.08
> XADD ticks 1-2 type stock sym AAPL px 190
> XADD ticks 1-3 type bond sym UST10Y px 98.5
> XADD ticks 1-4 type fx sym GBPUSD px 1.27

> XGROUP CREATE ticks fx 0 FILTER "type = fx AND px > 1.1"
OK
> XGROUP CREATE ticks rest 0 ELSE
OK

> XREADGROUP GROUP fx c1 STREAMS ticks >
ticks: 1-4                        # the one match
> XREADGROUP GROUP rest c1 STREAMS ticks >
ticks: 1-1, 1-2, 1-3              # what fx does not take

> XGROUP CREATE ticks big 0 FILTER "px > 1000"
OK
> XREADGROUP GROUP big c1 MAXSCAN 2 STREAMS ticks >
ticks: (empty array)              # cap reached before a match

> XINFO GROUPS ticks              # group big
pending 0  last-delivered-id 1-2  entries-read 2  lag 2  filter "px > 1000"  maxscan 10000

> XGROUP CREATE ticks bad 0 FILTER "type = fx AND"
(error) ERR invalid filter: syntax error at byte 13: expected a field name or '('
> XGROUP CREATE ticks rest2 0 ELSE
(error) ERR The stream already has an ELSE group
```

## Commands

| Change | Kind |
|---|---|
| `XGROUP CREATE ... [FILTER expr \| ELSE] [MAXSCAN n]` | new options |
| `XGROUP SETFILTER key group expr \| ELSE \| NONE [MAXSCAN n \| DEFAULT]` | new subcommand; takes effect from the next delivery |
| `XREADGROUP ... [MAXSCAN n]` | new option, next to `MAXCOUNT` and `MAXSIZE` |
| `XINFO GROUPS`, `XINFO STREAM FULL`: `filter`, `maxscan` | new reply fields, only for groups with a filter or a scan cap |
| `stream-filter-max-scan` | new config, default 10,000 |

- **Scan cap for a read:** the read's `MAXSCAN`, else the group's, else
  `stream-filter-max-scan`. `XINFO` reports the cap in effect.
- **`COUNT`:** counts deliveries, not entries examined.
- **`MAXSIZE`:** a match left out of the reply comes in the next read.
- **History:** reading the group's pending entries (an ID other than `>`)
  ignores the filter.

Definitions: `src/commands/xgroup-create.json`,
`src/commands/xgroup-setfilter.json`, `src/commands/xreadgroup.json`,
`src/config.c`.

## Filter grammar

From `src/streamfilter.h`:

```text
expr     := and-expr ( OR and-expr )*
and-expr := clause ( AND clause )*
clause   := field op value
          | field [NOT] IN "(" value ( "," value )* ")"
          | field [NOT] EXISTS
          | "(" expr ")"
op       := "=" | "!=" | "<" | "<=" | ">" | ">=" | "~"
```

- **Comparison type:** set by the literal. A bareword that reads as a number
  compares numerically; anything else, and every quoted literal, compares
  bytewise.
- **Missing field:** every clause is false except `NOT EXISTS`.
- **Repeated field:** the first occurrence is the value.
- **`~`:** substring match.
- **Limits:** 4,096 bytes per expression, 10,000 `IN` members, nesting
  depth 16. A parse error gives the byte offset.

## How it works

| Piece | Where |
|---|---|
| Parser and evaluator | `src/streamfilter.c`, `src/streamfilter.h` |
| `XGROUP CREATE`/`SETFILTER`, the read path, the wakeup check | `src/t_stream.c` |
| The filter on the group (`streamCG.filter`) and the wakeup state (`streamCG.wake_*`) | `src/stream.h` |
| RDB type 37, `RDB_TYPE_STREAM_LISTPACKS_6`, written only for streams with filters | `src/rdb.c`, `src/rdb.h` |
| AOF rewrite: the filter and cap on `XGROUP CREATE` | `src/aof.c` |
| Active defrag of filter memory | `src/defrag.c` |
| A blocked read asks the stream before running again | `src/blocked.c` |

**A filtered read** runs in two steps:

1. Skip the entries after the cursor that the group does not take, up to
   the first match or the scan cap.
2. Deliver matches, up to `COUNT`, with what is left of the cap.

**Field positions:** for entries flagged `SAMEFIELDS`, the filter finds its
fields once per listpack node and reads values by position.

**Blocked readers:** the wakeup check reads only the new entries, backward
from the stream's end. It records the range it found non-matching on the
group, so each entry is checked once per group. A woken read skips that
range without reading it again. Deletes, trims, `XSETID` and filter changes
keep the range correct or drop it.

**Memory:** the filter's allocations count in the stream's `alloc_size`, so
`MEMORY USAGE` and per-slot memory include them.

**Replication and AOF:** replicas never evaluate a filter. They receive the
writes native `XREADGROUP` sends: one `XCLAIM` per delivered entry, and
`XGROUP SETID` when the cursor moves without a delivery.

## Compatibility

- **Opt-in:** per group. Groups without a filter take the existing path plus
  one null check. On a stream without filtered groups, `XADD` evaluates no
  filter.
- **Pending entries list:** holds deliveries only, so `XPENDING`, `XACK`,
  `XCLAIM`, `XAUTOCLAIM` and `XNACK` don't change.
- **New reply:** an empty entry list when the scan cap runs out before a
  match. Only filtered groups return it.
- **`lag` on a filtered group:** counts entries not yet examined, not
  matches not yet delivered.
- **Storage:** streams with filters save as RDB type 37, which older servers
  cannot load; other streams save as before.

## Testing

`tests/unit/type/stream-filter.tcl` (42 tests) and one defrag test in
`tests/unit/memefficiency.tcl`:

```text
./runtest --single unit/type/stream-filter
./runtest --single unit/type/stream-filter --only '/model of their semantics' --accurate
```

One test is randomized. It runs random entries, filters, reads, blocked
reads and `SETFILTER`, plus `XDEL`, `XTRIM` and `XGROUP SETID` in a second
mode. After every step it checks each reply and each group's cursor against
a model of the semantics written in Tcl. `FM_SEED=<seed>` replays a round,
`FM_MODE` picks a mode, `FM_TRACE=1` prints every command.

| Build (macOS unless noted) | Result |
|---|---|
| Default | full suite: 9,193 tests, no failures (2026-11-10, `80c83d58b`) |
| `-Werror -DDEBUG_ASSERTIONS -DDEBUG_ASSERT_KEYSPACE` | `stream`, `stream-cgroups`, `stream-filter`: 495 pass; with `info-stream`, 570 (2026-11-10, `80c83d58b`) |
| `SANITIZER=address`, `-Werror -DDEBUG_ASSERTIONS` | the same four files: 570 pass, no sanitizer reports (2026-11-10, `80c83d58b`) |
| Randomized test, `--accurate` | 10 runs on the keyspace-assert build and 3 with ASAN, each 2 modes × 40 rounds × 1,000 steps: no failures (2026-11-10) |
| jemalloc, Linux arm64 (Docker) | `memefficiency`: the filter defrag test passes, and fails with the `streamFilterDefrag()` call disabled (2026-11-06, `1d37de970`) |

## Commits

| Commit | Change |
|---|---|
| `facfb4d3e` | Consumer group filters: `XGROUP CREATE ... FILTER/ELSE/MAXSCAN`, `XGROUP SETFILTER`, `XREADGROUP ... MAXSCAN`, RDB, AOF, `XINFO` |
| `85773adc5` | A blocked filtered read wakes only for entries its group takes |
| `804016e87` | Read filtered fields by position within each listpack node |
| `f200caf91` | Active defrag of filter memory |
| `eeca83044` | Tests for `MAXSIZE` on filtered reads |
| `1d37de970` | A woken read skips what the wakeup check already passed over |
| `90be090f6` | Wakeup state kept correct across deletions, trims and `XSETID` |
| `6ae373b18` | RDB load refuses a second `ELSE` group |
| `80c83d58b` | Randomized test against a model of the semantics |

## Not done

- Filtered reads without a group (`XREAD`, `XRANGE`). The reply would need a
  cursor, since a filtered scan can spend its budget without a match.
- Filter counters in `INFO`.
- A GCC `-Werror` build, and MSan and TSAN runs.
- The jemalloc defrag test on Linux at the latest commits.

## Open questions

- **RDB version:** `RDB_VERSION` stays at 17. A server at 17 without this
  change accepts a type-37 payload and then fails to load it.
- **`ELSE`:** in a first version, or later.
- **API:** once shipped, the grammar, the typing rule, the missing-field rule
  and `ELSE` are permanent.
