#!/usr/bin/env python3
"""Fixed G2 output checks, using only Python and throwaway output trees."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from check_g2_outputs import check_outputs  # noqa: E402

HELLO = "# 无量山冒烟测试\n这是第一次真实运行。\n完成"
MEETING = "## 背景\n周会。\n## 决定\n两人复核。\n## 待办\n周：11 月 17 日。\n陈：11月24日。\n## 风险\n紧急通道未决。\n"
CSV = "region,orders,total\n华东,4,4100\n华北,4,3850\n华南,4,2650\n"
FAQ = "".join(f"### 问题{i}：如何重置密码？\n请使用重置链接。\n" for i in range(1, 6)) + "## 修订记录\n压缩第 3 个答案。\n"
VALID = {"meeting.md": MEETING, "summary.csv": CSV,
         "summary.md": "华东金额最高，占总金额的38.7%。\n", "faq.md": FAQ}


class G2OutputsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.output = self.base / "output"
        self.output.mkdir()

    def write(self, files):
        for name, content in files.items():
            path = self.output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content.encode("utf-8") if isinstance(content, str) else content)

    def result(self, task="three", failed=None):
        result = check_outputs(self.output, task)
        self.assertEqual(result["task"], task)
        self.assertEqual(result["passed"], failed is None)
        self.assertEqual(result["verdict"], "PASS" if failed is None else "FAIL")
        self.assertEqual(result["passed"], all(c["passed"] for c in result["checks"].values()))
        if failed:
            self.assertFalse(result["checks"][failed]["passed"])
            self.assertTrue(result["checks"][failed]["reason"])
        else:
            for check in result["checks"].values():
                self.assertTrue(check["passed"])
                self.assertEqual(check["reason"], "")
        return result

    def test_smoke_exact_lines(self):
        for content in (HELLO, HELLO + "\n"):
            self.write({"node/hello.md": content})
            self.result("smoke")
        for content in (HELLO + "\n\n", "\n" + HELLO, HELLO + " ",
                        HELLO.replace("完成", "未完成"), HELLO.replace("\n", "\r\n")):
            with self.subTest(content=content):
                self.write({"node/hello.md": content})
                self.result("smoke", "hello.content")

    def test_each_three_rule(self):
        self.write(VALID)
        self.result()
        cases = [
            ("meeting.headings", "meeting.md", MEETING.replace("## 背景", "### 背景")),
            ("meeting.todo_周", "meeting.md", MEETING.replace("周：", "林：")),
            ("meeting.todo_周", "meeting.md", MEETING.replace("17 日", "18 日")),
            ("meeting.todo_陈", "meeting.md", MEETING.replace("陈：", "林：")),
            ("meeting.todo_陈", "meeting.md", MEETING.replace("24日", "25日")),
            ("meeting.risk", "meeting.md", MEETING.replace("紧急通道", "普通流程")),
            ("summary.csv_header", "summary.csv", CSV.replace("region", "area")),
            ("summary.csv_rows", "summary.csv", CSV.replace("4100", "4101")),
            ("summary.csv_rows", "summary.csv", CSV + "华东,1,2\n"),
            ("summary.csv_rows", "summary.csv", CSV.replace("华北,4,3850\n", "")),
            ("summary.csv_rows", "summary.csv", CSV.replace("华东,4,4100\n华北,4,3850", "华北,4,3850\n华东,4,4100")),
            ("summary.region", "summary.md", "华南占比38.7%。"),
            ("summary.percent", "summary.md", "华东占比38.6%。"),
            ("summary.percent", "summary.md", "华东占比138.7%。"),
            ("faq.questions", "faq.md", FAQ.replace("### 问题5", "## 问题5")),
            ("faq.questions", "faq.md", FAQ.replace("## 修订记录", "### 第六问\n回答。\n## 修订记录")),
            ("faq.third_answer", "faq.md", FAQ.replace("### 问题4", "汉" * 41 + "\n### 问题4")),
            ("faq.revision", "faq.md", FAQ.replace("## 修订记录", "## 说明")),
            ("faq.revision", "faq.md", FAQ + "## 其他\n内容。\n"),
            ("faq.revision", "faq.md", FAQ + "### 第六问\n内容。\n"),
        ]
        for rule, name, content in cases:
            with self.subTest(rule=rule, content=content):
                self.write({name: content})
                self.result(failed=rule)
                self.write({name: VALID[name]})

    def test_section_and_han_boundaries(self):
        long_question = "标题" * 40
        third = f"### {long_question}\n#### {long_question}\n" + "汉" * 40 + "，。！？0123456789abc\n"
        faq = FAQ.replace("### 问题3：如何重置密码？\n请使用重置链接。\n", third)
        self.write({**VALID, "faq.md": faq})
        self.result()
        self.write({"faq.md": faq.replace("汉" * 40, "\U00020000" * 41)})
        self.result(failed="faq.third_answer")
        self.write({"faq.md": FAQ.replace("### 问题3：如何重置密码？\n请使用重置链接。", "### 问题3：如何重置密码？")})
        self.result(failed="faq.third_answer")
        self.write({"faq.md": FAQ.replace("## 修订记录", "### 修订记录")})
        self.result()
        self.write({"meeting.md": MEETING.replace("周：11 月 17 日。", "周负责。") + "11 月 17 日。\n"})
        self.result(failed="meeting.todo_周")
        self.write({"meeting.md": "```markdown\n" + MEETING + "```\n"})
        self.result(failed="meeting.headings")

    def test_recursive_inventory(self):
        self.write({f"node{i}/{name}": content for i, (name, content) in enumerate(VALID.items())})
        self.result()
        for name in ("extra.txt", "other/faq.md"):
            self.write({name: "unexpected"})
            self.result(failed="files")
            (self.output / name).unlink()
        (self.output / "node3/faq.md").unlink()
        self.result(failed="files")

    def test_input_boundaries(self):
        self.write({"hello.md": HELLO})
        self.result("smoke")
        self.write({"extra": "extra"})
        self.result("smoke", "files")
        (self.output / "extra").unlink()
        outside = self.base / "outside"
        outside.mkdir()
        (outside / "secret.txt").write_text("outside", encoding="utf-8")
        for target in (outside, outside / "secret.txt"):
            (self.output / "linked").symlink_to(target)
            report = self.result("smoke", "files")
            self.assertNotIn("secret.txt", report["checks"]["files"]["reason"])
            (self.output / "linked").unlink()
        (self.output / "hello.md").unlink()
        (outside / "hello.md").write_text(HELLO, encoding="utf-8")
        (self.output / "hello.md").symlink_to(outside / "hello.md")
        report = self.result("smoke", "files")
        self.assertFalse(report["checks"]["hello.content"]["passed"])
        (self.output / "hello.md").unlink()
        self.write({"hello.md": HELLO})
        os.mkfifo(self.output / "fifo")
        self.result("smoke", "files")
        (self.output / "fifo").unlink()
        self.write({"hello.md": b"\xff"})
        self.result("smoke", "files")
        (self.output / "hello.md").unlink()
        self.result("smoke", "files")
        (self.base / "linked-output").symlink_to(self.output)
        for path in (self.base / "linked-output", self.base / "missing", outside / "secret.txt"):
            self.assertFalse(check_outputs(path, "smoke")["passed"])

    def test_cli_json_and_exit(self):
        self.write({"hello.md": HELLO})
        for expected in (0, 1):
            process = subprocess.run([sys.executable, str(ROOT / "scripts/check_g2_outputs.py"),
                                      str(self.output), "smoke"], capture_output=True, text=True)
            self.assertEqual(process.returncode, expected)
            self.assertEqual(process.stderr, "")
            self.assertEqual(json.loads(process.stdout)["passed"], expected == 0)
            self.write({"hello.md": "wrong"})


if __name__ == "__main__":
    unittest.main()
