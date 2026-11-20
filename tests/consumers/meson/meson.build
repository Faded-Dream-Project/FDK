project('fdk_meson_consumer', 'c',
  version: '0.1',
  default_options: ['c_std=c17'],
)

# The consumer-build test's Meson twin: dependency('fdk') rides
# the INSTALLED pkg-config file (never the source tree) — the
# .pc's integration test. The prefix arrives via PKG_CONFIG_PATH
# (+ SYSROOT for DESTDIR-staged trees) from the Makefile driver.

fdk_dep = dependency('fdk')

executable('consumer', 'main.c',
  dependencies: fdk_dep,
)
