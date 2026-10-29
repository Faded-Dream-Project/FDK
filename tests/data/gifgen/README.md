# The animated-GIF fixtures (tests/test_gif_fixtures.h)

Regenerate with a PIL-equipped Python:

    python3 gen_gifs.py     # matrix.gif (the disposal matrix) + single.gif
    python3 gen_hand.py     # interlaced.gif + delays.gif (byte-exact)

then rebuild `tests/test_gif_fixtures.h` by concatenating the emitted
`*.h` arrays under the guard (the file is committed so the suite stays
hermetic — no runtime dependency on PIL or on this directory).

- `matrix.gif` — PIL output against a REAL encoder: compressed LZW
  (table growth + KwKwK), per-frame transparency, disposal 1/2/3,
  NETSCAPE loop, and the embedded second data stream PIL's save_all
  writes mid-file (the resync case).
- `single.gif` — one frame (animation is not required).
- `interlaced.gif` — hand-written: the four-pass row order.
- `delays.gif` — hand-written: exact delays 0cs/1cs/2cs (the
  compatibility-floor pins) + NETSCAPE loop 3.
