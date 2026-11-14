#!/usr/bin/env python3
"""Check fixed G2 deliverables: python3 scripts/check_g2_outputs.py OUTPUT smoke|three.

Print per-rule JSON and exit 0 for PASS, 1 for FAIL. Uses only the standard
library; reads UTF-8 regular files below OUTPUT without following symlinks.
Checks the fixed requirements in claude-2026-11-14/04-next.md, not prose quality.
"""

import argparse
import csv
import io
import json
import os
from pathlib import Path
import re
import stat
import unicodedata

HELLO = "# 无量山冒烟测试\n这是第一次真实运行。\n完成"
THREE = {"meeting.md", "summary.csv", "summary.md", "faq.md"}
ROWS = [["华东", "4", "4100"], ["华北", "4", "3850"], ["华南", "4", "2650"]]


def read_output(output, expected):
    documents, found, errors = {}, {}, []

    def walk_error(error):
        raise error

    try:
        root_fd = os.open(output, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            for directory, dirs, files, dir_fd in os.fwalk(
                    ".", dir_fd=root_fd, follow_symlinks=False, onerror=walk_error):
                dirs.sort()
                for name in sorted(dirs + files):
                    relative = str(Path(directory) / name)
                    mode = os.stat(name, dir_fd=dir_fd, follow_symlinks=False).st_mode
                    if stat.S_ISDIR(mode):
                        continue
                    if not stat.S_ISREG(mode):
                        errors.append(f"{relative}: symlinks and non-regular files are forbidden")
                        continue
                    found.setdefault(name, []).append(relative)
                    if name not in expected:
                        continue
                    try:
                        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                                     dir_fd=dir_fd)
                        with os.fdopen(fd, "rb") as stream:
                            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                                raise ValueError("not a regular file")
                            documents[name] = stream.read().decode("utf-8")
                    except (OSError, ValueError) as error:
                        errors.append(f"{relative}: {error}")
        finally:
            os.close(root_fd)
    except OSError as error:
        errors.append(f"output directory: {error}")
    for name in sorted(expected):
        count = len(found.get(name, []))
        if count != 1:
            errors.append(f"{name}: expected one file, found {count}")
            documents.pop(name, None)
    for name in sorted(found.keys() - expected):
        errors.append(f"unexpected file: {', '.join(found[name])}")
    return documents, errors


def headings(text):
    """Locate ATX headings, excluding fenced code; keep offsets for section bodies."""
    result, offset, fence = [], 0, None
    for line in text.splitlines(keepends=True):
        marker = re.match(r"^ {0,3}(`{3,}|~{3,})", line)
        if fence:
            if re.fullmatch(r" {0,3}" + re.escape(fence[0]) + "{" + str(len(fence))
                            + r",}[ \t]*\r?\n?", line):
                fence = None
        elif marker:
            fence = marker[1]
        else:
            match = re.match(r"^ {0,3}(#{1,6})[ \t]+(.+?)\s*$", line)
            if match:
                title = re.sub(r"[ \t]+#+$", "", match[2])
                result.append((len(match[1]), title, offset, offset + len(line)))
        offset += len(line)
    return result


def section(text, marks, heading):
    end = next((h[2] for h in marks if h[2] > heading[2] and h[0] <= heading[0]), len(text))
    return text[heading[3]:end]


def check_outputs(output, task):
    if task not in ("smoke", "three"):
        raise ValueError("task must be smoke or three")
    documents, errors = read_output(output, {"hello.md"} if task == "smoke" else THREE)
    checks = {}

    def record(name, passed, reason):
        checks[name] = {"passed": bool(passed), "reason": "" if passed else reason}

    record("files", not errors, "; ".join(errors))
    if task == "smoke":
        record("hello.content", documents.get("hello.md") in (HELLO, HELLO + "\n"),
               "hello.md must contain exactly the three required lines, with at most one final LF")
    else:
        meeting = documents.get("meeting.md", "")
        marks = headings(meeting)
        titles = {h[1] for h in marks if h[0] == 2}
        record("meeting.headings", {"背景", "决定", "待办", "风险"} <= titles,
               "meeting.md needs the four H2 headings 背景, 决定, 待办, 风险")
        for title, name, day in (("待办", "周", 17), ("待办", "陈", 24)):
            body = "\n".join(section(meeting, marks, h) for h in marks if h[:2] == (2, title))
            record(f"meeting.todo_{name}", name in body and re.search(fr"(?<!\d)11\s*月\s*{day}\s*日", body),
                   f"meeting.md 待办 must mention {name} and 11 月 {day} 日")
        risk = "\n".join(section(meeting, marks, h) for h in marks if h[:2] == (2, "风险"))
        record("meeting.risk", "紧急通道" in risk, "meeting.md 风险 must mention 紧急通道")
        try:
            rows = list(csv.reader(io.StringIO(documents.get("summary.csv", "")), strict=True))
        except csv.Error:
            rows = []
        record("summary.csv_header", rows and rows[0] == ["region", "orders", "total"],
               "summary.csv header must be region,orders,total")
        record("summary.csv_rows", rows[1:] == ROWS,
               "summary.csv must have exactly three data rows: 华东,4,4100; 华北,4,3850; 华南,4,2650")
        summary = documents.get("summary.md", "")
        record("summary.region", "华东" in summary, "summary.md must mention 华东")
        record("summary.percent", re.search(r"(?<![\d.])38\.7%(?!\d)", summary),
               "summary.md must contain 38.7%")
        faq = documents.get("faq.md", "")
        marks = headings(faq)
        revisions = [h for h in marks if "修订记录" in h[1]]
        cutoff = revisions[0][2] if revisions else len(faq)
        questions = [h for h in marks if h[0] == 3 and h[2] < cutoff]
        record("faq.questions", len(questions) == 5,
               f"faq.md needs five H3 questions before 修订记录; found {len(questions)}")
        answer = section(faq, marks, questions[2]) if len(questions) >= 3 else ""
        for heading in reversed(headings(answer)):
            answer = answer[:heading[2]] + answer[heading[3]:]
        han_count = sum(unicodedata.name(c, "").startswith(
            ("CJK UNIFIED IDEOGRAPH-", "CJK COMPATIBILITY IDEOGRAPH-")) for c in answer)
        record("faq.third_answer", answer.strip() and han_count <= 40,
               f"faq.md third answer must be present and contain at most 40 Han characters; found {han_count}")
        final_revision = len(revisions) == 1 and marks[-1] == revisions[0]
        record("faq.revision", final_revision,
               "faq.md must end with one section whose heading contains 修订记录")
    passed = all(check["passed"] for check in checks.values())
    return {"task": task, "checks": checks, "passed": passed, "verdict": "PASS" if passed else "FAIL"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("task", choices=("smoke", "three"))
    args = parser.parse_args()
    result = check_outputs(args.output, args.task)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
