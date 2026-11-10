# Controls S0 arithmetic overflow fix — RED/GREEN/UBSan validation

Status: **S0_ARITHMETIC_RED_GREEN_QUALIFIED**. One bounded stage obtained a
qualified Release RED (25 passed / 2 target failures), applied the exact
checked-tail repair, reproduced GREEN in the same build tree (27/0) and ran
an independent Debug UBSan build (27/0, no diagnostics). This closes only
the S0 arithmetic slice; the complete Controls W/D/S/R feature set and its
full acceptance matrix remain **NOT_COMPLETE**.

Real execution time is UTC 2026-11-10, 01:59:00Z through 02:40:58Z (single
t0; total budget 4500 s, Release sub-budget 2400 s closed at about t0+1867 s,
UBSan local budget 2100 s used for ~650 s). The 20261109 in this filename is
the fixed task path binding chosen when the route was planned, not the
execution date.

## Change and identity

- `src/controls_detail.hpp`: before
  `68daf61f94b7f7851a2d055e7b09ab3c32cb52897dfde89a278dbd9e66c4017a`
  (28800 B, 709 lines) → after
  `c3a98a8161d1107efb94bc0901dcb0464d26d85a8ddf0443d022c5698d9bdb9c`
  (28836 B, 709 lines; net +36 B, zero net lines: the checked-multiplication
  guard line gained the checked addition as a second condition and the former
  unchecked tail-addition line was removed).
- The millisecond tail `nanoseconds / 1'000'000` is now added through
  `add_i64_checked` on the same guarded path; a failure returns the original
  typed `time_status::overflow`. No domain change (minutes stay 1..10^12), no
  clamping, no deleted assertion, no new algorithm.
- `tests/controls_detail_tests.cpp` (T) stayed byte-identical to the
  reviewed snapshot `113b81921d5e862825c5aa9747a83248610b02af7a03511811e2cf48dbbd1890`
  (27 registered cases); every other source/build-registration file stayed
  byte-identical to the published snapshot this stage started from.
- Fixed production comparison base
  `4987f2c6a6fc0da4b91e53d3689019ba3f0e8631` (never swapped for the snapshot
  commit). Full working-tree recount after the fix: production
  added+deleted = 790+4 = **794 ≤ 800** (tests 772+0; this report is
  documentation and is not counted as production lines).

## Validation route and results

Toolchain: GCC 13.3.0 (`/usr/bin/g++-13`), CMake/CTest 3.28.3, one own cold
Release build (`-G "Unix Makefiles"`, `-DCMAKE_BUILD_TYPE=Release`,
`-O3 -DNDEBUG`, UBSan/ASan OFF), CTest selection exactly one registration
`^controls_detail$`, original TIMEOUT 90 unchanged, CTest parallel 1,
compiler parallel 2, sanitized environment (no implicit flags, launchers or
foreign caches), outer umask 0022, UID/GID 1000/1000, stage-private
temporary directory.

- **RED (unfixed header, same T):** 25 passed / 2 failed, ctest exit 8. The
  only failures were `remaining_milliseconds_floor_tail_808_overflows` and
  `remaining_milliseconds_floor_tail_999_overflows`, each exactly at the
  original `remaining.status == time_status::overflow` CHECK; the INT64_MAX
  tail case (`remaining_milliseconds_floor_int64_max_tail_807`) and the other
  24 cases passed.
- **RED preservation:** before any mutation the complete Release build tree
  (748 files: ELF, objects, static libraries, depfiles, link command,
  CMakeCache/compile_commands, generated rules and the full RED Testing
  outputs) was physically copied to an independent archive — no hardlinks,
  no symlinks, byte-verified per file with distinct inode identities. The
  RED junit, show-only JSON and Testing directory originals are frozen
  separately.
- **GREEN (same build tree, same flags, same T):** after the fix the two
  translation units that depend on the header (`controls_clock_posix.cpp.o`,
  `controls_detail_tests.cpp.o`) were recompiled, proven by the real compiler
  invocations in the verbose log plus new object mtimes after the header
  mtime — not by hashes alone. `libwls_s1.a` was re-archived (real
  `/usr/bin/ar` invocation; its bytes remained identical because the clock
  translation unit does not use the changed inline function — the case the
  plan explicitly anticipated), while the test translation unit's object and
  the final ELF changed. Result: **27 passed / 0 failed**, ctest exit 0. The
  frozen `green-source` copy (681 files) is byte-identical to the fixed
  workspace; the workspace header, the mutable validation copy and
  green-source are identical.
- **UBSan (independent fresh build from green-source):** Debug with the
  original `-fsanitize=undefined -fno-sanitize-recover=undefined` flags
  unchanged, run with `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`:
  **27 passed / 0 failed and no UBSan diagnostic anywhere in the outputs**.

Per-operation expanded argv, environment allowlists, UTC and monotonic
start/end times, exits, raw stdout/stderr, and per-file SHA/size/mode/identity
manifests are retained in the stage-local private evidence directory and are
not committed. This S0 stage made no Store/SQL/TLS/provider calls: business
DB and wire originals are NOT_APPLICABLE for it, not fabricated empties.

## Scope and publication limits

- This stage wrote only `src/controls_detail.hpp` and this report. No Git
  commit, push, PR or merge happened in this stage. Publishing the fix
  requires the plan's fresh independent hard review and the same-author
  exact follow-on publication record; open PRs including drafts remain
  capped at three and merging stays with the owner.
- Complete Controls W/D/S/R behavior, Correction qualification and the full
  matrix (non-root Release, dual umask, Clang, full ASan/UBSan runs, original
  Make and full CTest, legacy/SingleAgent/clean31) remain open. Store 410,
  the published SQL, permission/UNKNOWN/claim/budget/deadline assertions and
  the original 90-second per-suite timeouts are unchanged.
- Windows 11 remains **NOT_RUN**. Private evidence hosting remains
  **BLOCKED**. Real or paid WLS provider calls remain **0**.
