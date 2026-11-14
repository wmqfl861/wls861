"""Explicit shell configuration for legacy CLI tool suites, before admission."""
from pathlib import Path
import shutil


def select_legacy_shell(functions):
    path = functions / 'general/function.yaml'
    text = path.read_text(encoding='utf-8')
    assert '  shell:\n' in text and '  default:\n' in text
    path.write_text(text.replace('    isolation: standard\n', '    isolation: strong\n')
                       .replace('  default:\n', '  files_original:\n', 1)
                       .replace('  shell:\n', '  default:\n', 1), encoding='utf-8')


def legacy_shell_arguments(arguments, source, directory):
    args = list(arguments)
    if not args or args[0] != 'submit':
        return args
    original = source / 'config/examples/functions'
    index = args.index('--functions-dir') + 1 if '--functions-dir' in args else None
    if index is not None and Path(args[index]) != original:
        return args  # A test's explicit custom configuration owns its grants.
    functions = directory / 'legacy-shell-functions'
    if not functions.exists():
        shutil.copytree(original, functions)
        select_legacy_shell(functions)
    if index is None:
        args.extend(['--functions-dir', functions])
    else:
        args[index] = functions
    return args
