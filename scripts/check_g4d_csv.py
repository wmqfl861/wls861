"""Exact oracle for the fixed G4-D CSV submission; no runtime claim is inferred."""
import json
from pathlib import Path
import sys
from check_g2_outputs import read_output

EXPECTED = (Path(__file__).resolve().parents[1] / "tests/fixtures/g4/csv-expected.csv").read_text(encoding="utf-8")


def check_output(directory):
    documents, errors = read_output(directory, {"summary.csv"})
    if documents.get("summary.csv") != EXPECTED:
        errors.append("summary.csv differs from the independent fixed oracle")
    return {"passed": not errors, "errors": errors}


if __name__ == "__main__":
    result = check_output(sys.argv[1])
    print(json.dumps(result, ensure_ascii=False))
    raise SystemExit(0 if result["passed"] else 1)
