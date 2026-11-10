# Context source candidate saved without runtime acceptance

Recorded 2026-11-10. This commit saves the reviewed Context source candidate. The source-only review and narrow type-comparison correction review passed within their stated scopes. **Runtime acceptance remains FAIL_OPEN; full Context remains OPEN.** Saving and pushing this source does not establish runtime acceptance or new feature completion.

The latest local attempt completed configuration, eleven compilation commands and eleven link commands, each with native exit status 0. Its private dependency verifier then rejected the lexical source alias tests/../src/worker_tools.cpp; this was a verifier identity-gate failure. Full closure of all 49 object dependencies and 14 ELF runtime identities, test discovery and the selected twenty-test batch were not reached. **All 20 tests are NOT_RUN; zero business methods were reached.** The supervisor recorded a maximum observation gap of 37.69433836796088 seconds, exceeding the required 30 seconds, with six other gaps also above 30 seconds. Stable before/post identity evidence is NOT_RUN: an outer collection status of COMPLETE does not establish that identity check.

Candidate binding: reviewed HEAD **18f16afc574a33460064fe644cc381e2e9eeeca3**; legal Protocol parent **01c330d697fc2eda928495218a7ba7464dab5a22**; frozen 688-member content-manifest SHA-256 **537bb91d88c17848dbc8c603e600ff0bee7e633a9731755756105c135ab40944**, including 126 test files and src/history_readback_internal.hpp. Cumulative production additions plus deletions remain **716/800**. This publication adds no production or test changes beyond that frozen reviewed candidate. This report is outside the runtime manifest; the source-save commit is the commit that first adds this report.

Existing artifacts are retained for one separately reviewed, bounded validation tail without production recompilation or relinking; no tail results are claimed here. The historical permission gap for 33 input aliases resolving to 30 targets remains UNKNOWN. Earlier RED failures, GREEN01 build exit 2, the old COMPILE_C1 native-return UNKNOWN and the old mode-seal FAIL remain unchanged. Current observations cannot reconstruct missing historical evidence.

Worker v2 text execution and file-read projection; the 60 KB/at-most-three-reads, reopen, EOF and next-input requirements; remaining negative cases and 31+8 consumer cases; and complete local-six/specified-remote-six validation remain open where not executed. Historical TLS 19/19 belongs to RED03 and does not validate this candidate. Windows 11 is NOT_RUN, private evidence hosting is BLOCKED, and paid provider validation is NOT_RUN. No real provider spending, main merge or additional pull request follows from this source save.

Read-only source identity reproduction from this branch (Python 3 and Git; runs no product code):

```python
import hashlib, json, subprocess

def git(*args):
    return subprocess.check_output(["git", *args])

report = "docs/reports/g1-context-unverified-candidate-20261110.md"
rev = git("log", "--diff-filter=A", "--format=%H", "--", report).decode().splitlines()[0]
manifest = {}
for entry in git("ls-tree", "-rz", "--full-tree", rev).split(b"\0"):
    if not entry:
        continue
    meta, path = entry.split(b"\t", 1)
    if path.decode() == report:
        continue
    mode, kind, oid = meta.decode().split()
    assert kind == "blob"
    data = git("cat-file", "blob", oid)
    manifest[path.decode()] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
raw = json.dumps(manifest, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()
assert len(manifest) == 688
assert hashlib.sha256(raw).hexdigest() == "537bb91d88c17848dbc8c603e600ff0bee7e633a9731755756105c135ab40944"
print(rev, hashlib.sha256(raw).hexdigest())
```
