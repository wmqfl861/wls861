"""Retain one original native CTest run and verify its complete method output.

JUnit may truncate successful output at 1024 bytes. Never reconstruct missing
method outcomes from source or rerun tests to fill an original evidence gap.
"""
import ast
import hashlib
import json
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET


def retain_logs(build, evidence):
    original = build / 'Testing/Temporary'
    destination = evidence / 'ctest-logs'
    destination.mkdir(exist_ok=False)
    files = {}
    for name in ('LastTest.log', 'LastTest.log.tmp', 'LastTestsFailed.log', 'CTestCostData.txt'):
        source = original / name
        if source.is_file():
            target = destination / name
            shutil.copyfile(source, target)
            raw = source.read_bytes()
            if target.read_bytes() != raw:
                raise RuntimeError('original CTest log changed during capture: ' + name)
            files[name] = {'source': str(source), 'size_bytes': len(raw),
                           'sha256': hashlib.sha256(raw).hexdigest()}
    (evidence / 'ctest-log-files.json').write_text(json.dumps({'files': files,
        'complete_lasttest_present': 'LastTest.log' in files,
        'partial_lasttest_present': 'LastTest.log.tmp' in files}, indent=2) + '\n', encoding='utf-8')


def expected_methods(source):
    tests = source / 'platform/native-foundation/tests'
    result = {}
    for suite, name in (('native_modules', 'modules_tests.cpp'), ('native_platform', 'platform_tests.cpp')):
        text = (tests / name).read_text(encoding='utf-8')
        bodies = text.split('return native_test::run({')
        if len(bodies) != 2:
            raise RuntimeError('method evidence requires one explicit native test list: ' + name)
        result[suite] = re.findall(r'\{"([^"\n]+)",', bodies[1].split('\n    });', 1)[0])
    for suite, name, class_name in (('native_cli', 'test_native_cli.py', 'NativeCLI'),
                                    ('native_bundle_capture', 'test_bundle_capture.py', 'BundleCapture')):
        module = ast.parse((tests / name).read_text(encoding='utf-8'), filename=name)
        classes = [node for node in module.body if isinstance(node, ast.ClassDef) and node.name == class_name]
        if len(classes) != 1:
            raise RuntimeError('method evidence requires the original unittest class: ' + name)
        result[suite] = [node.name for node in classes[0].body if isinstance(node, ast.FunctionDef)
                         and node.name.startswith('test_')]
    if any(not methods or len(set(methods)) != len(methods) for methods in result.values()):
        raise RuntimeError('method evidence requires nonempty unique original method names')
    return result


def verify_methods(source, evidence):
    expected = expected_methods(source)
    lasttest = evidence / 'ctest-logs/LastTest.log'
    raw = lasttest.read_bytes()
    text = raw.decode('utf-8').replace('\r\n', '\n')
    cases = ET.parse(evidence / 'ctest.xml').findall('.//testcase')
    failures = []
    if sorted(case.get('name') for case in cases) != sorted(expected):
        failures.append('JUnit suite set differs from the original four native registrations')
    if any(case.get('status', 'run') != 'run' or any(case.find(tag) is not None
           for tag in ('failure', 'error', 'skipped')) for case in cases):
        failures.append('JUnit has a failed, skipped or unexecuted suite')
    blocks = re.split(r'(?m)^\d+/\d+ Testing: ', text)[1:]
    names = [block.split('\n', 1)[0] for block in blocks]
    if sorted(names) != sorted(expected):
        failures.append('complete LastTest suite set differs from the original four registrations')
    suites = []
    for suite, methods in expected.items():
        matches = [block for name, block in zip(names, blocks) if name == suite]
        passed = []
        if len(matches) == 1:
            parts = re.split(r'(?m)^Output:\n-+\n', matches[0], maxsplit=1)
            output, separator, trailer = parts[-1].partition('<end of output>\n')
            if len(parts) != 2 or not separator or not re.search(r'(?m)^Test Passed\.', trailer):
                failures.append(suite + ': incomplete or unsuccessful original LastTest block')
            if suite in ('native_modules', 'native_platform'):
                passed = re.findall(r'(?m)^PASS (.+)$', output)
                if not re.search(r'(?m)^RESULT ' + str(len(methods)) + r' passed, 0 failed$', output):
                    failures.append(suite + ': missing original native result summary')
            else:
                passed = re.findall(r'(?m)^(test_\w+) \([^\n]+\) \.\.\. ok$', output)
                if not re.search(r'(?m)^Ran ' + str(len(methods)) + r' tests in [0-9.]+s$', output) \
                        or not re.search(r'(?m)^OK$', output):
                    failures.append(suite + ': missing complete unittest summary')
        if sorted(passed) != sorted(methods):
            failures.append(suite + ': missing, duplicate, unknown or nonpassing method')
        suites.append({'suite': suite, 'expected_methods': methods,
                       'passed_methods': methods if sorted(passed) == sorted(methods) else passed,
                       'emitted_passed_methods': passed, 'expected_method_count': len(methods)})
    receipt = {'status': 'FAILED' if failures else 'PASSED', 'suite_count': len(expected),
               'method_count': sum(len(methods) for methods in expected.values()),
               'lasttest_sha256': hashlib.sha256(raw).hexdigest(), 'suites': suites,
               'failures': failures, 'method_evidence_source': 'original complete LastTest.log',
               'junit_method_output_may_be_truncated': True}
    (evidence / 'ctest-methods.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    if failures:
        raise RuntimeError('incomplete native method evidence: ' + '; '.join(failures))
    return receipt
