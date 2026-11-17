"""Fixed FakeModel program; its stdout describes bytes read in this process."""
import csv
import errno
import hashlib
import io
import json
from pathlib import Path
import sys

source, target = map(Path, sys.argv[1:3])
raw = source.read_bytes()
write_errno = None
if sys.argv[3:] == ["--check-read-only"]:
    try:
        with source.open("ab") as stream:
            stream.write(b"forbidden")
    except OSError as error:
        write_errno = error.errno
    assert write_errno in (errno.EROFS, errno.EACCES, errno.EPERM), "input write succeeded"
rows = csv.reader(io.StringIO(raw.decode("utf-8")), strict=True)
assert next(rows) == ["region", "amount"]
totals = {}
for region, amount in rows:
    count, total = totals.get(region, (0, 0))
    totals[region] = (count + 1, total + int(amount))
with target.open("x", encoding="utf-8", newline="") as stream:
    writer = csv.writer(stream, lineterminator="\n")
    writer.writerow(["region", "orders", "total"])
    writer.writerows((region, *totals[region]) for region in sorted(totals, key=lambda key: (-totals[key][1], key)))
print(json.dumps({"input_bytes": len(raw), "input_sha256": hashlib.sha256(raw).hexdigest(),
                  "output_sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "input_write_errno": write_errno}))
