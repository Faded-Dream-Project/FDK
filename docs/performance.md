# FDK Performance Baseline

Phase 11's profiling deliverable. The harness is `tests/bench.c`
(`make bench`): headless, release-built (the default build is
ASan+UBSan-instrumented — sanitizer overhead would distort numbers by
multiples, so `bench` links a dedicated `build/libfdk-rel.a`).

The numbers exist to catch REGRESSIONS (an order-of-magnitude drop
between releases), not to chase single-digit percents — the project
principle against premature optimization applies until a profile
says otherwise. Re-run `make bench` on comparable hardware when
touching these paths.

## Baseline (reference container, 2026-08-28)

Tree fixture: nested H/V boxes, 475 widgets, labels with real text,
800x600. `FDK 1.0.0`, system DejaVuSans @16px, GCC -O2.

| Benchmark | Result | Notes |
|---|---|---|
| tree-churn (eager) | ~558 ms/tree | create+destroy, 475 widgets |
| tree-churn (batched) | ~1.1 ms/tree | **515x faster**, same final geometry |
| layout-sweep | ~0.8 ms/sweep | full measure+arrange of the 475-widget tree |
| paint-full | ~12 ms/frame | full-tree repaint to an offscreen surface |
| paint-damage | ~46 µs/frame | one-widget damage repaint (the steady-state case) |
| text-measure | ~10 µs/op | 123-byte paragraph, cached glyphs |
| text-break | ~10 µs/op | line-count pass @300px |
| event-motion | ~77 ns/event | hit-test + hover synthesis + bubbling |
| theme-switch | ~1.2 µs/swap | invalidate every live root |
| i18n-fmt-int | ~43 ns/op | de locale, grouped |
| i18n-fmt-double | ~229 ns/op | 2 fraction digits |
| i18n-plural | ~23 ns/op | de one/other rule |
| i18n-catalog | ~2 ns/op | NULL-catalog miss path |

## Findings and what was done about them

### 1. Quadratic tree construction — FIXED (layout batching)

Every child mutation relayouted the parent AND propagated upward, so
every ancestor container re-ran its full subtree layout. Building a
nested UI widget-by-widget was O(n²)-ish: 558 ms for 475 widgets.

`fdk_layout_begin_batch()/fdk_layout_end_batch()` (public,
`fdk_layout.h`) switch the notifier to marking mode — dirty bits up
the chain, one flush per chain's topmost container, whose arrange
cascade refreshes every descendant. Outside a batch, behavior is
byte-identical to the eager path (the whole existing suite pins
that). `tests/test_layout.c` proves batched == eager geometry,
nested-batch depth counting, unbalanced-end tolerance, and
mid-batch destroy safety (destroyed widgets are forgotten from the
pending set; a batch may span widget lifetimes).

**515x** on the fixture; the batching tests pin correctness.

### 2. paint-full is memory-bandwidth, not algorithm

12 ms/frame for the full repaint is dominated by overdraw: every
box background fills its bounds and children paint over them, so a
full-damage frame writes each pixel many times (~38 MB of pixel
traffic at 800x600 with the fixture's nesting). At ~3 GB/s effective
fill bandwidth the number is what the primitive costs, not a
bookkeeping bug. The mitigations are the ones every toolkit uses and
FDK already has: damage tracking (the steady-state 46 µs case is
260x cheaper than full), and avoiding full invalidations (which FDK
only does on resize/expose/theme-switch by design). An occlusion
cull (skip widgets fully covered by later opaque siblings) is the
known next step IF a profile ever shows full repaints dominating a
real app — recorded here as the honest frontier, not implemented on
speculation.

### 3. The steady-state numbers are healthy

46 µs damage repaints, 77 ns event dispatch, 23 ns plural rules,
1.2 µs whole-tree theme invalidation — the paths a 60 fps
interactive frame actually executes are microseconds, not
milliseconds. No further work indicated by this baseline.

## How to re-run

```
make bench          # builds build/bench against release objects
./build/bench
```

Compare like-for-like only (same machine, same build type). The
harness prints ops/s and ns/op per row; the fixture shapes are in
`tests/bench.c` and deliberately stable.
