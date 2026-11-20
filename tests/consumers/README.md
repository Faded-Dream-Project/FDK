# Consumer build-system tests

Two minimal downstream projects — one CMake, one Meson — built,
linked and RUN against an INSTALLED FDK, not the source tree.
`make consumer-test` drives the whole sequence:

1. build FDK in release config;
2. install to a throwaway prefix under `build/consumer-prefix/`;
3. configure + build + run each consumer against that prefix
   (CMake through `find_package(fdk CONFIG)`, the config package;
   Meson through `dependency('fdk')`, the pkg-config file);
4. the consumer exits nonzero on any failure.

The point: the install surface (headers, .so + SONAME symlink,
pkg-config, CMake config) is exercised exactly the way a
downstream integrator consumes it — a regression in the .pc's
paths, the config package's layout resolution, or the link set
fails HERE, not at the first external user.

Run standalone: `make consumer-test`. Requires cmake and meson
(both detected; the target skips with a clear message when a
tool is missing so it can ride along in dev environments).
