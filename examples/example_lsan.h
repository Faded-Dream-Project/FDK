/*
 * example_lsan.h — the sanitized examples' leak-strictness bind
 * (Round 75, 2026-11-12; the GTX "it still crashes" report)
 *
 * WHY THIS EXISTS: the owner's real-hardware runs of the sanitized
 * examples end with LeakSanitizer's exit report naming the GPU
 * driver's own process-lifetime state (libnvidia-glcore 1.9 MB,
 * libdbus ~750 B, libEGL_nvidia 256 B) — third-party blocks the
 * repo documents in tests/data/lsan-gl.supp. The report appears
 * because the run had no LSAN_OPTIONS set, and asking every run
 * to remember an env prefix is a footgun the toolkit can remove.
 *
 * THE MECHANISM: LeakSanitizer calls __lsan_default_options() in
 * the MAIN EXECUTABLE during its flag initialization — BEFORE
 * main() runs — and merges the returned flags as defaults;
 * LSAN_OPTIONS from the environment is applied afterwards and
 * always wins. An operator who exports LSAN_OPTIONS (their own
 * list, or nothing) overrides the bind; a bare run gets the
 * repo's list.
 *
 * THE EARLY-INIT LAW (learned the hard way this round, with a
 * core dump, a hand-rolled NT_PRSTATUS parser, and a ~15-program
 * bisect): default_options runs while the ASan runtime is still
 * initializing — ASanInitIsRunning() is true — and ANY libc call
 * from it re-enters the interceptor layer before that layer is
 * ready. The symptoms vary by timing: read/readlink/snprintf/
 * syscall die with an instant SIGSEGV before the first line of
 * output; access() got further and died on the interceptor's own
 * CHECK failed: asan_interceptors.cpp:509 "!AsanInitIsRunning()"
 * the moment a probe actually hit. There is NO safe libc subset.
 * This function must be a pure computation on compile-time data:
 * no readlink, no access, no snprintf, not even getpid. Hence the
 * make-baked absolute path (CPPFLAGS' FDK_EXAMPLE_LSAN_SUPP,
 * the repo root at parse time) instead of any runtime discovery.
 *
 * THE SUPPRESSIONS= FOOTGUN (Round 74's find): packing
 * suppressions= inside ASAN_OPTIONS fails at parse time on this
 * stack ("AddressSanitizer: failed to parse suppressions" —
 * 'leak:<module>' entries are LSan-channel syntax; the flag must
 * travel in LSAN_OPTIONS). The Makefile's sanitized targets
 * already use the LSAN_OPTIONS form; this header extends the same
 * contract to bare command-line runs.
 *
 * WHAT THIS DOES NOT DO: suppress anything FDK owns. The list
 * matches third-party module frames only; an FDK-frame leak still
 * fails the run (proven by scripts/s28_lsan_strictness_probe.c:
 * a deliberate malloc with an exe-frame stack reports and exits 1
 * with the bind active, and a self-module suppression entry
 * proves the machinery engages at all).
 *
 * NON-SANITIZED BUILDS: the whole body is compiled out unless
 * __SANITIZE_ADDRESS__ is defined, so release builds never see it.
 * If FDK_EXAMPLE_LSAN_SUPP is not defined (a hand-rolled cc line
 * outside the Makefile), the function returns the empty string —
 * no defaults, today's exact behavior.
 */

#ifndef FDK_EXAMPLE_LSAN_H
#define FDK_EXAMPLE_LSAN_H

#ifdef __SANITIZE_ADDRESS__

#ifndef FDK_EXAMPLE_LSAN_SUPP
#define FDK_EXAMPLE_LSAN_SUPP ""
#endif

/* Pure: returns a pointer to compile-time data (either the make-
 * baked string constant or the empty literal). Zero calls, zero
 * writes, zero re-entrancy — the early-init law above. LSan calls
 * this exactly once, during flag init, before main. */
const char *__lsan_default_options(void);

const char *__lsan_default_options(void) {
    return "suppressions=" FDK_EXAMPLE_LSAN_SUPP;
}

#endif /* __SANITIZE_ADDRESS__ */

#endif /* FDK_EXAMPLE_LSAN_H */
