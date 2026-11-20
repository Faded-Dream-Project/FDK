# Quality gates (CI and local)

This document covers the quality gates added 2026-11-20: the fuzz
lane, the ABI check, the clang-tidy report, hardened release flags,
the advisory perf gate, and the arm64 lane. All are wired into CI
(`.github/workflows/quality-gates.yml`) and runnable locally.

## Fuzzing

Six libFuzzer harnesses live in `tests/fuzz/`, one per untrusted-input
parser: `.fdk` themes, `.fmo` catalogs, `.prefs` stores, PNG/GIF
(stb), SVG (nanosvg), and the D-Bus wire parser. Build and run:

```
make fuzz CC=clang FUZZ_SECONDS=20        # every harness, 20s each
FUZZ_HARNESS=theme make fuzz CC=clang ... # one harness
```

The harnesses need clang (libFuzzer) and rebuild the archive with
clang when the tree was built by gcc, so sanitizer coverage reaches
the parsers themselves. Seeds are committed under
`tests/fuzz/corpus/<name>/`, regenerable from in-tree fixtures via
`python3 tests/fuzz/gen_seeds.py`.

**Longer runs.** CI runs each harness for a fixed 60 s per push —
that is a smoke, not a campaign. For real coverage, run each for
hours (or overnight) locally:

```
./build/fuzz/fuzz_theme -max_total_time=3600 \
    -rss_limit_mb=2560 -malloc_limit_mb=1100 \
    -artifact_prefix=build/fuzz/artifacts/ \
    tests/fuzz/corpus/fuzz_theme
```

Findings (crash/leak/timeout/oom inputs) land in
`build/fuzz/artifacts/`. Reduce a finding by replaying it:

```
./build/fuzz/fuzz_theme build/fuzz/artifacts/crash-xxxx
```

**The lane's first finds** (2026-11-20, both fixed in the same
commit that added the lane): a 27-byte GIF claiming 24836×14080
OOMed the decoder (stb's stock dimension cap was 2²⁴; now pinned
to 16384 in `stb_image_impl.c`), and a "legal" 16132×16191 frame
still allocated ~2.2 GB through GIF's triple buffer (now
pre-flighted by a header-only pixel budget of 64 Mi pixels in
`surface_image.c`).

## ABI check

`tools/abi/libfdk.so.1.baseline.xml` is the committed abidw dump of
the public ABI; `tools/abi/check-abi.sh` diffs the built `libfdk.so`
against it (requires libabigail). Run via `make abi-check`; CI runs
it on every push and fails on a breaking change.

**Updating the baseline for an intended append-only change:**

1. `make shared`
2. `abidw --out-file tools/abi/libfdk.so.1.baseline.xml build/libfdk.so`
3. `git diff tools/abi/` and verify the diff only **adds** — new
   functions, new types. A removal or signature change is an ABI
   break by definition (`docs/abi-policy.md`); the SONAME major
   must move instead.

**The 2026-11-20 baseline refresh** (the `fdk_init_ex` contract,
below) is the worked example of step 3: `abidiff` reported exactly
`1 Added function` — `fdk_init_ex` — with 0 removed, 0 changed, and
no struct layout change. That is what an append-only change looks
like on the wire; the refreshed baseline commits it.

### fdk_init_options and the append-only input contract

`fdk_init_options` grew twice by appending (`render_mode`, then
`image_max_pixels`) with no way for FDK to know which layout a
caller compiled against — a caller built against the smaller
struct passes a smaller allocation, and FDK reading the newer
fields is an immediate over-read. The fix is a SIZE-DISCIPINED
ENTRY POINT, not a struct change:

- `fdk_init(ctx, opts)` is frozen forever as "opts points at the
  full current layout" — its ABI and behavior are unchanged.
- `fdk_init_ex(ctx, opts, opts_size)` reads each field only if it
  lies wholly inside `opts_size` (`offsetof`-checked per field);
  `opts_size = 0` means the full layout; a larger size clamps.
  A caller compiled against the first-generation 16-byte layout
  can never be over-read, and newer fields read as their
  zero-init defaults.

Why not the leading `struct_size` the toplevel/widget-class/ext
tables use? Those types were BORN with the field; `fdk_init_options`
was not, and prepending it shifts every existing field's offset —
an immediate break of the exact ABI this gate protects. The full
reasoning lives in `fdk_core.h`'s `fdk_init_ex` comment; the
16-byte-caller regression test (under ASan) is
`tests/test_core.c::test_init_ex_size_contract`.

## clang-tidy

`make tidy` (advisory, never fails the build) runs clang-tidy over
the core sources and writes `build/clang-tidy.report`. CI publishes
the report as a job summary. Findings are triaged by hand; the
current run's real findings are listed in the CI job summary — most
are naming/const suggestions, not defects. When the noise floor is
known, the `|| true` can be tightened into a blocking gate.

## Hardened release flags

`FDK_CONFIG=release` now builds with `-D_FORTIFY_SOURCE=3`
(falls back to =2 on compilers without 3), `-fstack-protector-strong`,
`-fPIC -fPIE`, and links with `-Wl,-z,relro,-z,now` (verified in the
built `.so`: `BIND_NOW` in `readelf -d`). PIE executables get
`-pie` through `REL_LDFLAGS` when they link the release config.
Verified: the dlopen'd lanes (harfbuzz/fribidi, XI2, Xrender, GL)
are unaffected — none of these flags touch dynamic-loader behavior
— and `make test` / `make test-x11` stay green.

## Perf gate (advisory)

`make perf-gate` runs the bench harness and compares ns/op against
`tests/perf-baseline.txt` with a 2× tolerance. It is **advisory by
design**: CI runners are shared machines and the noise level is not
yet known; the gate prints regressions but does not fail. Once the
noise level is measured across some real CI runs, tighten the
tolerance and flip the exit code to blocking. Re-baseline after
intentional performance changes: `make bench`, then regenerate
`tests/perf-baseline.txt` from the bench output.

## arm64 lane

CI builds and runs `make test` on aarch64 via the
`ubuntu-24.04-arm` GitHub runner (native, no qemu). Local
equivalent without ARM hardware: the cross toolchain + qemu route
documented in `scripts/` (gcc-aarch64 + qemu-user static, static
link). Status at commit time: the arm64 CI lane is wired and
expected green on the runner; no local arm64 hardware was available
to pre-verify, so first-run results come from CI itself.

## What is deliberately NOT here yet

- OSS-Fuzz integration (needs a public repo decision).
- Coverity scan (needs project registration).
- The blocking perf gate (needs noise measurement first).
- clang-tidy as a hard gate (needs a triaged finding list first).
