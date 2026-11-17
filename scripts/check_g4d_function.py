"""Check captured source bytes and original unittest outcome without executing artifacts."""
from pathlib import Path
import re
from check_g2_outputs import read_output

FIXTURE = Path(__file__).resolve().parents[1] / "tests/fixtures/g4"
EXPECTED = {name: (FIXTURE / name).read_text(encoding="utf-8") for name in ("clamp.py", "test_clamp.py")}


def check_output(directory, exit_code, stdout, stderr):
    documents, errors = read_output(directory, set(EXPECTED))
    if documents != EXPECTED:
        errors.append("captured source files differ from the fixed independent oracle")
    if exit_code != 0 or stdout or not re.search(rb"^Ran 5 tests in [0-9.]+s\n\nOK\n\Z", stderr, re.MULTILINE):
        errors.append("original execution must exit zero with five successful unittest cases")
    return {"passed": not errors, "errors": errors}
