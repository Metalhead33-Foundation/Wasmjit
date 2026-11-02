#!/usr/bin/env python3
"""CM-9a -- extract the component-model corpus into artifacts a test can load.

The corpus under ``extern/component-model/test`` is written for ``wasmtime wast``:
WAT *script* syntax, which ``wasm-tools json-from-wast`` cannot read (it uses the
core parser and stops at the first ``(canon ...)`` form).  This walks each script,
takes every top-level ``(component ...)`` and every ``(assert_invalid (component
...) ...)``, and writes each through ``wasm-tools parse`` -- which *does*
understand components.  Output is a manifest the C++ test reads:

    <verdict> <artifact> <origin> <reason>

``accept`` = the script says the component is valid, ``reject`` = ``assert_invalid``,
``skip`` = the form could not be extracted, or ``wasm-tools`` itself refused it,
so one unparsable case never breaks the build.  Everything is written under
``--out``; the source tree is untouched.
"""

import argparse
import ast
import os
import re
import subprocess
import sys


def scan(text):
    """Yield (start, end) for every top-level s-expression, comment/string aware."""
    i, n, depth, start = 0, len(text), 0, None
    while i < n:
        if text.startswith(';;', i):
            while i < n and text[i] != '\n':
                i += 1
            continue
        if text.startswith('(;', i):
            level = 1
            i += 2
            while i < n and level:
                if text.startswith('(;', i):
                    level += 1
                    i += 2
                elif text.startswith(';)', i):
                    level -= 1
                    i += 2
                else:
                    i += 1
            continue
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == '(':
            if depth == 0:
                start = i
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0 and start is not None:
                yield start, i + 1
                start = None
        i += 1


def head(text, start):
    j = start + 1
    while j < len(text) and text[j].isspace():
        j += 1
    k = j
    while k < len(text) and not text[k].isspace() and text[k] not in '()':
        k += 1
    return text[j:k]


def balanced(text, start):
    """Index just past the form starting at `start`, or None when unbalanced."""
    depth = 0
    i, n = start, len(text)
    while i < n:
        if text.startswith(';;', i):
            while i < n and text[i] != '\n':
                i += 1
            continue
        if text.startswith('(;', i):
            level = 1
            i += 2
            while i < n and level:
                if text.startswith('(;', i):
                    level += 1
                    i += 2
                elif text.startswith(';)', i):
                    level -= 1
                    i += 2
                else:
                    i += 1
            continue
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return None


def first_component(text, start, end):
    at = text.find('(component', start, end)
    while at != -1:
        after = at + len('(component')
        if after >= len(text) or text[after].isspace() or text[after] in '()':
            stop = balanced(text, at)
            if stop is not None:
                return text[at:stop]
        at = text.find('(component', after, end)
    return None


def parse_invoke(form):
    """Return (name, args) for one no-nested-command invoke, or None."""
    at = form.find('(invoke')
    if at < 0:
        return None
    stop = balanced(form, at)
    if stop is None:
        return None
    text = form[at:stop]
    name = re.search(r'\(invoke\s+"([^"]+)"', text)
    if not name:
        return None
    args = []
    for kind, value in re.findall(
            r'\((u32|i32|u64|i64|f32|f64|char)\.const\s+("(?:[^"\\]|\\.)*"|[^()\s]+)\)',
            text):
        if kind == 'char':
            try:
                args.append(('char', '0x%x' % ord(ast.literal_eval(value))))
            except (SyntaxError, ValueError, TypeError):
                continue
        else:
            args.append((kind, value))
    return name.group(1), args


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--corpus', required=True)
    parser.add_argument('--tools', default='wasm-tools')
    parser.add_argument('--out', required=True)
    parser.add_argument('--known-failures', default=None,
                        help='curation file copied next to the manifest')
    parser.add_argument('--skip-file', action='append', default=[],
                        help='curation file, one corpus-relative path per line')
    args = parser.parse_args()

    corpus = os.path.abspath(args.corpus)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    curated = set()
    for skip_file in args.skip_file:
        if os.path.exists(skip_file):
            with open(skip_file, encoding='utf-8') as handle:
                curated.update(line.strip() for line in handle
                               if line.strip() and not line.startswith('#'))

    scripts = []
    for root, _dirs, files in os.walk(corpus):
        for name in files:
            if name.endswith('.wast'):
                scripts.append(os.path.join(root, name))
    scripts.sort()

    manifest = []
    execution = []
    counts = {'accept': 0, 'reject': 0, 'skip': 0}
    form_path = os.path.join(out, 'form.wat')
    for script in scripts:
        relative = os.path.relpath(script, corpus)
        if relative in curated:
            manifest.append(('skip', '-', relative, 'listed in the curation file'))
            counts['skip'] += 1
            continue
        with open(script, encoding='utf-8', errors='replace') as handle:
            text = handle.read()
        index = 0
        current_artifact = None
        for start, end in scan(text):
            form_head = head(text, start)
            verdict = None
            component = None
            if form_head == 'component':
                verdict, component = 'accept', text[start:end]
            elif form_head == 'assert_invalid':
                component = first_component(text, start, end)
                verdict = 'reject' if component else None
            if verdict is None or component is None:
                # A top-level assert_return/assert_trap belongs to the most
                # recent top-level component. Scalar arguments/results are
                # representable without a host adapter.
                form = text[start:end]
                if form_head in ('invoke', 'action') and current_artifact:
                    parsed = parse_invoke(form)
                    if parsed:
                        arg_text = ','.join('%s:%s' % item for item in parsed[1])
                        execution.append((current_artifact, parsed[0],
                                          'action', '',
                                          '%s:%d' % (relative, start), arg_text))
                elif form_head == 'assert_return' and current_artifact:
                    parsed = parse_invoke(form)
                    # Only constants after the invoke form are expected
                    # results; constants inside the invoke are arguments.
                    invoke_at = form.find('(invoke')
                    invoke_end = balanced(form, invoke_at) if invoke_at >= 0 else -1
                    expected = re.findall(
                            r'\((bool|u8|s8|u16|s16|u32|i32|u64|i64|f32|f64)\.const\s+([^()\s]+)\)',
                            form[invoke_end:] if invoke_end >= 0 else '')
                    if parsed and expected:
                        arg_text = ','.join('%s:%s' % item for item in parsed[1])
                        kind, value = expected[-1]
                        if kind == 'bool':
                            value = '1' if value == 'true' else '0'
                        execution.append((current_artifact, parsed[0], kind,
                                          value, '%s:%d' % (relative, start),
                                          arg_text))
                    elif parsed:
                        # An `assert_return` with no expected result is an
                        # action for a void export; emit it as one so the
                        # harness still performs the call.
                        arg_text = ','.join('%s:%s' % item for item in parsed[1])
                        execution.append((current_artifact, parsed[0],
                                          'action', '',
                                          '%s:%d' % (relative, start), arg_text))
                elif form_head == 'assert_trap' and current_artifact:
                    parsed = parse_invoke(form)
                    if parsed:
                        arg_text = ','.join('%s:%s' % item for item in parsed[1])
                        execution.append((current_artifact, parsed[0],
                                          'trap', '',
                                          '%s:%d' % (relative, start), arg_text))
                continue
            index += 1
            stem = os.path.splitext(os.path.basename(script))[0]
            artifact = '%s_%03d.wasm' % (stem, index)
            with open(form_path, 'w', encoding='utf-8') as handle:
                handle.write(component)
            result = subprocess.run(
                    [args.tools, 'parse', form_path, '-o', os.path.join(out, artifact)],
                    capture_output=True, text=True)
            origin = '%s:%d' % (relative, start)
            if result.returncode != 0:
                reason = (result.stderr.strip().splitlines() or ['parse failed'])[0]
                manifest.append(('skip', '-', origin, reason))
                counts['skip'] += 1
                current_artifact = None
                continue
            manifest.append((verdict, artifact, origin, ''))
            counts[verdict] += 1
            current_artifact = artifact if verdict == 'accept' else None

    manifest_path = os.path.join(out, 'manifest.tsv')
    with open(manifest_path, 'w', encoding='utf-8') as handle:
        handle.write('# verdict\tartifact\torigin\treason (CM-9a)\n')
        for entry in manifest:
            handle.write('\t'.join(entry) + '\n')
    execution_path = os.path.join(out, 'execution.tsv')
    with open(execution_path, 'w', encoding='utf-8') as handle:
        handle.write('# artifact\tinvoke\tkind\tvalue\torigin\targs (CM-9a)\n')
        for entry in execution:
            handle.write('\t'.join(entry) + '\n')
    if args.known_failures and os.path.exists(args.known_failures):
        import shutil
        shutil.copyfile(args.known_failures,
                        os.path.join(out, 'component_known_failures.txt'))
    print('component corpus: %d accept, %d reject, %d skip -> %s'
          % (counts['accept'], counts['reject'], counts['skip'], manifest_path))
    return 0


if __name__ == '__main__':
    sys.exit(main())
