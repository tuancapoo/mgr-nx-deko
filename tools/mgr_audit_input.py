"""Expand post-codegen marker macros for the source dataflow audit only.

The production generated tree is never changed. These are the enabled macro
forms in sa2_pch.h; the audit otherwise mistakes output operands and saved
register names for reads, hiding real findings behind thousands of markers.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/experiments/full_codegen_20261016/audit_input'
MARKER = re.compile(r'REX_(LEAF_LOCAL|LEAF_STORE|LOOP_LOCAL|LOOP_IN|LOOP_OUT)\(([^()]*)\)')


def marker(m):
    op = m[1]
    args = [a.strip() for a in m[2].split(',')]
    if op == 'LEAF_LOCAL':
        ty, name, reg = args
        return f'{ty} {name} = ctx.{reg}'
    if op == 'LEAF_STORE':
        name, reg = args
        return f'ctx.{reg} = {name}'
    if op == 'LOOP_LOCAL':
        ty, name, _ = args
        return f'{ty} {name}{{}}'
    name, reg = args
    return f'{name} = {reg}' if op == 'LOOP_IN' else f'{reg} = {name}'


def expand(text):
    # Reverse post-pass aliases for dataflow. Merely expanding copy-in makes a
    # dead initial value look like a real hidden argument: these auditors do not
    # follow the ll_* temporary. The production macros remain untouched.
    aliases = {}
    def undo_alias(m):
        op = m[1]
        args = [a.strip() for a in m[2].split(',')]
        if op in ('LEAF_LOCAL', 'LOOP_LOCAL'):
            _, name, reg = args
            aliases[name] = reg if reg.startswith('ctx.') else 'ctx.' + reg
        return ''
    text = MARKER.sub(undo_alias, text)
    if aliases:
        names = re.compile(r'\b(' + '|'.join(map(re.escape, aliases)) + r')\b')
        text = names.sub(lambda m: aliases[m[0]], text)
    lines = []
    for line in text.splitlines():
        # These frame-traffic elisions remain OFF in the production build.
        for macro in ('REX_DEAD_SAVE', 'REX_DEAD_LR_SAVE', 'REX_BACK_CHAIN'):
            match = re.fullmatch(r'(\s*)' + macro + r'\((.*)\);', line)
            if match:
                line = match[1] + match[2] + ';'
        line = re.sub(r'REX_LR_READ\(((?:ctx\.)?\w+)\)', r'\1.u64 = ctx.lr', line)
        line = re.sub(r'REX_LR_WRITE\(((?:ctx\.)?\w+)\)', r'ctx.lr = \1.u64', line)
        # CR is an output of CompareAnyMode; spell it as the comparator the
        # existing audit understands (this file is analyzed, never compiled).
        line = re.sub(r'rex::ppc::CompareAnyMode\((cr\d), (.*)\);',
                      r'\1.compare<double>(\2, xer);', line)
        stripped = line.strip()
        if stripped.startswith('{ ') and stripped.endswith(' }'):
            lines.extend('\t' + part.strip() + ';'
                         for part in stripped[2:-2].split(';') if part.strip())
        else:
            lines.append(line)
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    count = 0
    for source in (ROOT / 'app/generated/default').glob('*_recomp.*.cpp'):
        (OUT / source.name).write_text(expand(source.read_text()), encoding='utf-8')
        count += 1
    print(f'Normalized {count} audit-only source files into {OUT}')
