# Bundled libvterm

- Upstream: https://www.leonerd.org.uk/code/libvterm/ (Paul Evans), MIT License (see `LICENSE`).
- Version: **0.3.3**, from `libvterm-0.3.3.tar.gz`
  (sha256 `09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0`).
- Imported unmodified: `include/`, `src/` (including the pre-generated `src/encoding/*.inc`
  and `src/fullwidth.inc`), and `LICENSE`. Upstream's `Makefile`, `bin/` and `t/` were not imported.
- Built as a static library by `third_party/libvterm/CMakeLists.txt` (zterminal's own file).

To update: replace `include/` and `src/` with a new release, bump the version and hash here,
and run the test suite.
