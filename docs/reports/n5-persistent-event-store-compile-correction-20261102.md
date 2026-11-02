# N5-1 Store remote compile correction — 2026-11-02

The initial Store backend source commit
`88c8259776a0b2773287fbe03abc7049db4c2c36` was normally pushed and published as
[draft PR 19](https://github.com/wmqfl861/wls861/pull/19), stacked on the exact
design branch from PR 16. Its original source report retains its publication
time scope and local **C++_BUILD_NOT_RUN / PRODUCT_TESTS_NOT_RUN** status.

The first remote strict job
[125302872841](https://github.com/wmqfl861/wls861/actions/runs/41453094570/job/125302872841)
failed while compiling `src/store.cpp`. GCC produced 39 primary diagnostics in
the new included protocol source: 38 unsupported comparisons between a
`std::string` and a `boost::json::value`, plus one `-Werror=shadow` diagnostic
for the second binding-source local. The Store product methods were not
reached. This is a real compile failure; the earlier static SQL checks did not
detect these C++ type errors.

The correction explicitly constructs a JSON string for each of those 38 text
operands, preserving the intended strict JSON equality/inequality. It renames
the second binding-source local to `payload_binding`. No transaction, guard,
identity, canonical data, SQL, resource hash, test assertion or suite deadline
is removed or relaxed. Migration 013 remains unchanged.

A source normalization check removed JSON constructor wrappers from both
versions and reverted that one local rename, then confirmed byte-identical
remaining source. `git diff --check` also passed. The corrected protocol source
SHA-256 is
`74bbb598046be90cd192ea19ccb6a5c92f10479578d84afa8dc46e4c046a5c85`.
This is source evidence, not a successful C++ compilation.

Private evidence under `evidence/n5-1-store-publish-01/` preserves the original
remote job log, the initial CLI refusal to print terminal escape sequences,
the successful raw download, and a separate readable copy. The original raw
download is 1,651,446 bytes. The primary compiler diagnostics, exact minimal
patch and source-equivalence receipt are retained under
`evidence/n5-1-store-compile-correction-01/`.

At this correction's source checkpoint, local C++ compilation and all 17 new
Store methods remain **NOT_RUN**. Validation of the correction by another
remote build is pending publication. The complete crash matrix, runtime
integration, full regressions, sanitizers, formal review and merge remain
separate incomplete states. This report does not overwrite the original
failure or claim implementation acceptance.
