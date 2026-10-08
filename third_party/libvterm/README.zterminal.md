# Bundled libvterm

- Upstream: https://www.leonerd.org.uk/code/libvterm/ (Paul Evans), MIT License (see `LICENSE`).
- Version: **0.3.3**, from `libvterm-0.3.3.tar.gz`
  (sha256 `09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0`).
- Imported unmodified except for the thirteen local patches listed below: `include/`, `src/` (including the pre-generated `src/encoding/*.inc`
  and `src/fullwidth.inc`), and `LICENSE`. Upstream's `Makefile`, `bin/` and `t/` were not imported.
- Built as a static library by `third_party/libvterm/CMakeLists.txt` (zterminal's own file).

## Local patches

Thirteen small fixes, found by fuzzing (`fuzz/fuzz_terminal`) with
ASan/UBSan. All are reachable from program output (the first two followed by a
window resize), so a remote host could crash zterminal or make it read out of
bounds. Each is marked `zterminal patch` in the source:

1. **Out-of-bounds read** (`screen.c`, `resize_buffer()`): the reflow loop walks back over continuation rows
   with `old_row >= 0`; a continuation flag on row 0 (its first half already
   in scrollback) walks to row -1 and reads before the buffer. Now stops at 0.
2. **abort()** (`screen.c`, `resize_buffer()`): when reflow leaves the cursor unset, upstream prints
   "screen_resize failed to update cursor position" and calls `abort()`
   (reproduced with a 1x1 screen, output `55`, resize to 3x3). The patch clamps
   the cursor into the new screen instead, there and when it is stored.
3. **NULL write** (`screen.c`): `putglyph()` marks the cell right of a double-width glyph
   without checking that it exists; on a 1-column screen a wide character
   writes through NULL. Now skipped when there is no such cell.
4. **Signed overflow** (`parser.c`): CSI numeric arguments and OSC command
   numbers were multiplied without bound (undefined behaviour for a long digit string). They now
   saturate at about 10^9, far beyond any meaningful parameter.
5. **Heap buffer overflow** (`parser.c`): each `;` or `:` in a CSI sequence
   increments the argument index without checking `CSI_ARGS_MAX` (16), so
   `ESC [` followed by many semicolons writes past `args[]` and beyond the
   `VTerm` allocation. This is remotely triggerable memory corruption. Excess
   arguments are now dropped, the same fix as Vim patch 9.2.0279
   (upstream libvterm is unfixed as of 0.3.3).
6. **Negative-size memmove** (`state.c`, `on_resize()`): shrinking the
   window clamps the bottom of a scroll region (DECSTBM) but not the top, so
   the region could become empty or inverted and the next line feed called
   `memmove` with a negative length (crash). Invalid regions are now reset
   after a resize, as DECSTBM already does for invalid input.
7. **Length underflow** (`parser.c`, end of `vterm_input_write()`): inside an
   OSC/DCS string, `ESC` followed by a C0 control as the last bytes of a read
   left an empty pending fragment whose length was decremented to `SIZE_MAX`.
   The title callback then read far past the buffer. The decrement is now
   guarded, as the same adjustment already is in the main loop.
8. **Infinite loop** (`state.c`, REP `CSI b`): with no preceding graphic
   character (fresh screen, or after the state is reset) the glyph width is 0
   and the repeat loop never advances, hanging the UI thread. REP is now
   ignored in that case.
9. **Heap buffer overflow** (`state.c`, `savecursor()`): DECSC (`ESC 7`)
   saves the cursor, but a later shrink does not clamp the saved position, so
   DECRC (`ESC 8`, also used by `?1049`) restored a row past the new bottom
   and the next character wrote `lineinfo[row]` out of bounds. The restored
   position is now clamped to the screen.
10. **Signed shift** (`state.c`, DECRQSS): bytes of the request were shifted
    as (possibly negative) `char`, which is undefined behaviour; now
    `unsigned char`. Harmless in practice, fixed so UBSan stays quiet.
11. **NULL memmove** (`screen.c`, `moverect_internal()`): `getcell()` returns
    NULL for a cell outside the screen and the result went straight to
    `memmove`; now such rows are skipped.
12. **Negative cursor column** (`state.c`, `on_text()`): a C1 control sent as
    UTF-8 text (`\xc2\x82`, U+0082) has width -1 and moved the cursor to
    column -1; a following TBC (`CSI g`) then wrote before the tab-stop array
    (heap overflow). Negative widths now count as 0 (upstream only aborts
    in DEBUG builds).
13. **Negative cursor column** (`vterm_internal.h`, `ROWWIDTH`): a
    double-width or double-height line (`ESC # 3..6`) on a 1-column screen had
    width `1 / 2 = 0`, so clamping the cursor put it at column -1 and erase
    and insert walked off the screen. The row width is now at least 1.

Re-apply them (or check that upstream fixed them) when updating. Covered by
`tst_terminal::resizeAfterOutputDoesNotAbort`, `wideCharInOneColumn`,
`hugeCsiArguments`, `tooManyCsiArguments`, `scrollRegionAfterShrink`, `oscEscC0AtChunkEnd`, `repWithoutPreviousChar`,
`restoreCursorAfterShrink`, `utf8C1DoesNotMoveCursorBack` and
`doubleWidthLineInOneColumn` (run
under ASan/UBSan in CI) and by the fuzz job.

To update: replace `include/` and `src/` with a new release, bump the version and hash here,
and run the test suite.
