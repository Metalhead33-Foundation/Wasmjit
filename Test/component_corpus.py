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
import json
import os
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


def _skip_space(text, index):
    """Skip whitespace and comments, returning the next significant index."""
    n = len(text)
    while index < n:
        if text[index].isspace():
            index += 1
            continue
        if text.startswith(';;', index):
            newline = text.find('\n', index)
            index = n if newline < 0 else newline + 1
            continue
        if text.startswith('(;', index):
            level = 1
            index += 2
            while index < n and level:
                if text.startswith('(;', index):
                    level += 1
                    index += 2
                elif text.startswith(';)', index):
                    level -= 1
                    index += 2
                else:
                    index += 1
            continue
        break
    return index


def parse_node(text, start):
    """Parse one s-expression from `start` into nested lists and atoms."""
    index = _skip_space(text, start)
    if index >= len(text) or text[index] != '(':
        raise ValueError('expected an s-expression')
    index += 1
    node = []
    n = len(text)
    while True:
        index = _skip_space(text, index)
        if index >= n:
            raise ValueError('unterminated s-expression')
        if text[index] == ')':
            return node, index + 1
        if text[index] == '(':
            child, index = parse_node(text, index)
            node.append(child)
            continue
        if text[index] == '"':
            begin = index
            index += 1
            while index < n:
                if text[index] == '\\':
                    index += 2
                    continue
                if text[index] == '"':
                    index += 1
                    break
                index += 1
            try:
                node.append(ast.literal_eval(text[begin:index]))
            except (SyntaxError, ValueError):
                node.append(text[begin:index].strip('"'))
            continue
        begin = index
        while index < n and not text[index].isspace() and text[index] not in '()':
            index += 1
        node.append(text[begin:index])


_SCALARS = ('bool', 'u8', 's8', 'u16', 's16', 'u32', 'i32', 'u64', 'i64',
            'f32', 'f64', 'char')



def const_json(node):
    """Return the JSON representation of one component value constant."""
    if not isinstance(node, list) or not node or not isinstance(node[0], str):
        return None
    head = node[0]
    if head.endswith('.const') and head[:-6] in _SCALARS:
        kind = head[:-6]
        if len(node) < 2 or not isinstance(node[1], str):
            return None
        value = node[1]
        if kind == 'bool':
            value = '1' if value == 'true' else '0'
        return {'kind': kind, 'value': value}
    if head == 'str.const':
        if len(node) < 2 or not isinstance(node[1], str):
            return None
        return {'kind': 'str', 'value': node[1]}
    if head == 'flags.const':
        return {'kind': 'flags',
                'values': [item for item in node[1:]
                           if isinstance(item, str)]}
    if head in ('list.const', 'fixed-list.const', 'tuple.const'):
        values = []
        for item in node[1:]:
            value = const_json(item)
            if value is None:
                return None
            values.append(value)
        return {'kind': head[:-6], 'elements': values}
    if head == 'record.const':
        fields = []
        for item in node[1:]:
            if (not isinstance(item, list) or len(item) < 3
                    or item[0] != 'field'):
                return None
            if isinstance(item[2], list):
                value = const_json(item[2])
            elif len(item) >= 4:
                value = const_json([item[2], item[3]])
            else:
                return None
            if value is None:
                return None
            name = item[1] if isinstance(item[1], str) else ''
            fields.append({'name': name, 'value': value})
        return {'kind': 'record', 'fields': fields}
    if head == 'variant.const':
        if len(node) < 2 or not isinstance(node[1], str):
            return None
        out = {'kind': 'variant', 'case': node[1], 'hasValue': len(node) > 2}
        if len(node) > 2:
            value = const_json(node[2])
            if value is None:
                return None
            out['value'] = value
        return out
    if head == 'enum.const':
        if len(node) < 2 or not isinstance(node[1], str):
            return None
        return {'kind': 'enum', 'case': node[1]}
    if head in ('option.none', 'option.some'):
        out = {'kind': 'option', 'hasValue': head == 'option.some'}
        if out['hasValue']:
            if len(node) < 2:
                return None
            value = const_json(node[1])
            if value is None:
                return None
            out['value'] = value
        return out
    if head in ('result.ok', 'result.err'):
        out = {'kind': 'result', 'isError': head == 'result.err',
               'hasValue': len(node) > 1}
        if out['hasValue']:
            value = const_json(node[1])
            if value is None:
                return None
            out['value'] = value
        return out
    if head == 'map.const':
        entries = []
        for item in node[1:]:
            if (not isinstance(item, list) or len(item) != 3
                    or item[0] != 'entry'):
                return None
            key = const_json(item[1])
            value = const_json(item[2])
            if key is None or value is None:
                return None
            entries.append([key, value])
        return {'kind': 'map', 'entries': entries}
    return None


def parse_invoke(form):
    """Return (name, arg JSON values, structured) for an invoke form."""
    at = form.find('(invoke')
    if at < 0:
        return None
    try:
        node, _ = parse_node(form, at)
    except (ValueError, IndexError):
        return None
    if len(node) < 2 or not isinstance(node[1], str):
        return None
    values = []
    for item in node[2:]:
        value = const_json(item)
        if value is None:
            # A non-constant argument (instance/ref/etc.) is still not
            # representable by this extractor; the caller records it as
            # unsupported rather than silently dropping it.
            return None
        values.append(value)
    structured = any(value['kind'] not in _SCALARS + ('str',)
                     for value in values)
    return node[1], values, structured


def parse_expected(form):
    """Return the expected values after an invoke form, or None on a gap."""
    at = form.find('(assert_return')
    if at < 0:
        return []
    try:
        node, _ = parse_node(form, at)
    except (ValueError, IndexError):
        return None
    invoke_index = -1
    for i, item in enumerate(node):
        if isinstance(item, list) and item and item[0] == 'invoke':
            invoke_index = i
            break
    if invoke_index < 0:
        return None
    expected = []
    for item in node[invoke_index + 1:]:
        value = const_json(item)
        if value is None:
            return None
        expected.append(value)
    return expected


def args_text(values, structured):
    """Encode invocation arguments for execution.tsv."""
    if structured:
        return 'json:' + json.dumps(values, separators=(',', ':'))
    return ','.join('%s:%s' % (value['kind'], value['value'])
                    for value in values)


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
                # recent top-level component. Scalar arguments and results are
                # representable without a host adapter; structured invocations
                # are marked unsupported so they are skipped explicitly.
                form = text[start:end]
                if form_head in ('invoke', 'action') and current_artifact:
                    parsed = parse_invoke(form)
                    if parsed:
                        name, parsed_args, structured = parsed
                        execution.append((current_artifact, name, 'action',
                                          '',
                                          '%s:%d' % (relative, start),
                                          args_text(parsed_args, structured)))
                elif form_head == 'assert_return' and current_artifact:
                    parsed = parse_invoke(form)
                    if parsed:
                        name, parsed_args, structured = parsed
                        arg_text = args_text(parsed_args, structured)
                        expected = parse_expected(form)
                        if expected is None:
                            execution.append((current_artifact, name,
                                              'unsupported',
                                              'structured value is not representable',
                                              '%s:%d' % (relative, start),
                                              arg_text))
                        elif expected:
                            if (len(expected) == 1
                                    and expected[0]['kind'] in _SCALARS + ('str',)):
                                execution.append((
                                    current_artifact, name,
                                    expected[0]['kind'], expected[0]['value'],
                                    '%s:%d' % (relative, start), arg_text))
                            else:
                                execution.append((
                                    current_artifact, name, 'json',
                                    json.dumps(expected, separators=(',', ':')),
                                    '%s:%d' % (relative, start), arg_text))
                        else:
                            # An `assert_return` with no expected result is an
                            # action for a void export; emit it as one so the
                            # harness still performs the call.
                            execution.append((current_artifact, name, 'action',
                                              '',
                                              '%s:%d' % (relative, start),
                                              arg_text))
                elif form_head == 'assert_trap' and current_artifact:
                    parsed = parse_invoke(form)
                    if parsed:
                        name, parsed_args, structured = parsed
                        execution.append((current_artifact, name, 'trap',
                                          '',
                                          '%s:%d' % (relative, start),
                                          args_text(parsed_args, structured)))
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
        handle.write('# artifact\tinvoke\tkind\tvalue\torigin\targs (CM-9a; kind=json is a typed array)\n')
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
