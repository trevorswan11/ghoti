"""Seed programs for the mutation fuzzer, and the token-level mutations applied to them."""
import glob
import os
import random
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))) + '/'
RAW = re.compile(r'R"(\w*)\((.*?)\)\1"', re.S)


def programs():
    """Valid-looking ghoti programs harvested from test raw strings, examples and std."""
    out = []
    for f in sorted(glob.glob(ROOT + 'lib/compiler/tests/**/*.cc', recursive=True)):
        txt = open(f, encoding='utf-8', errors='replace').read()
        for i, m in enumerate(RAW.finditer(txt)):
            body = m.group(2)
            if len(body) < 40 or ('fn' not in body and 'struct' not in body):
                continue
            # formatter-style tests double braces for fmt; skip those
            if '{{' in body and '}}' in body:
                body = body.replace('{{', '{').replace('}}', '}')
            rel = os.path.relpath(f, ROOT + 'lib/compiler/tests')[:-3]
            out.append((rel.replace(os.sep, '.').replace('/', '.') + '_' + str(i), body))
    for f in glob.glob(ROOT + 'examples/*.gh'):
        out.append(('ex_' + os.path.basename(f)[:-3], open(f, encoding='utf-8').read()))
    return out


TOKEN = re.compile(r'\s+|[A-Za-z_@][A-Za-z_0-9]*|\d+|"(?:[^"\\\n]|\\.)*"|.', re.S)


def tokens(src):
    return [m.group() for m in TOKEN.finditer(src)]


def mutate(src, rng):
    toks = tokens(src)
    if len(toks) < 4:
        return src
    kind = rng.randrange(7)
    i = rng.randrange(len(toks))
    if kind == 0:  # truncate
        return ''.join(toks[:i])
    if kind == 1:  # delete token
        return ''.join(toks[:i] + toks[i + 1:])
    if kind == 2:  # duplicate token
        return ''.join(toks[:i] + [toks[i]] + toks[i:])
    if kind == 3:  # swap with random other token
        j = rng.randrange(len(toks))
        toks[i], toks[j] = toks[j], toks[i]
        return ''.join(toks)
    if kind == 4:  # replace with random token from pool
        pool = ['(', ')', '{', '}', '[', ']', ';', ',', '.', ':', '=', '..', '^', '&', '?', '!',
                'fn', 'struct', 'union', 'enum', 'match', 'if', 'else', 'return', 'break',
                'continue', 'loop', 'for', 'while', 'defer', 'errdefer', 'const', 'var',
                'constexpr', 'pub', 'type', 'void', 'noreturn', 'i32', 'u8', 'bool', 'true',
                'undefined', '0', '1', '-1', '"s"', '@as', '@sizeOf', '@typeInfo', '@This',
                '@compileError', 'mut', 'dyn', 'impl', 'interface', 'self', '...', '_', '=>',
                '|x|', 'null', 'test', 'import', 'extern', 'export']
        toks[i] = ' ' + rng.choice(pool) + ' '
        return ''.join(toks)
    if kind == 5:  # delete a span
        j = min(len(toks), i + rng.randrange(1, 12))
        return ''.join(toks[:i] + toks[j:])
    # splice a span from elsewhere
    j = rng.randrange(len(toks))
    k = min(len(toks), j + rng.randrange(1, 12))
    return ''.join(toks[:i] + toks[j:k] + toks[i:])


def mutated_cases(seed, per_program, limit=None):
    rng = random.Random(seed)
    progs = programs()
    if limit:
        rng.shuffle(progs)
        progs = progs[:limit]
    out = []
    for name, src in progs:
        for k in range(per_program):
            out.append(('m%d_%s_%d' % (seed, name, k), mutate(src, rng)))
    return out
