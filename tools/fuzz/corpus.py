"""Seed programs for the mutation fuzzer, and the token-level mutations applied to them."""

import glob
import os
import random
import re

ROOT = (
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))) + "/"
)
RAW = re.compile(r'R"(\w*)\((.*?)\)\1"', re.DOTALL)


def programs():
    """Valid-looking ghoti programs harvested from test raw strings, examples and std."""
    out = []
    for f in sorted(glob.glob(ROOT + "lib/compiler/tests/**/*.cc", recursive=True)):
        with open(f, encoding="utf-8", errors="replace") as cc_file:
            txt = cc_file.read()
            for i, m in enumerate(RAW.finditer(txt)):
                body = m.group(2)
                if len(body) < 40 or ("fn" not in body and "struct" not in body):
                    continue
                # formatter-style tests double braces for fmt; skip those
                if "{{" in body and "}}" in body:
                    body = body.replace("{{", "{").replace("}}", "}")
                rel = os.path.relpath(f, ROOT + "lib/compiler/tests")[:-3]
                out.append(
                    (rel.replace(os.sep, ".").replace("/", ".") + "_" + str(i), body)
                )
    for f in glob.glob(ROOT + "examples/*.gh"):
        with open(f, encoding="utf-8") as gh_file:
            out.append(("ex_" + os.path.basename(f)[:-3], gh_file.read()))
    return out


TOKEN = re.compile(r'\s+|[A-Za-z_@][A-Za-z_0-9]*|\d+|"(?:[^"\\\n]|\\.)*"|.', re.DOTALL)


def tokens(src):
    return [m.group() for m in TOKEN.finditer(src)]


def mutate(src, rng):
    toks = tokens(src)
    if len(toks) < 4:
        return src
    kind = rng.randrange(8)
    i = rng.randrange(len(toks))
    if kind == 0:  # truncate
        return "".join(toks[:i])
    if kind == 1:  # delete token
        return "".join(toks[:i] + toks[i + 1 :])
    if kind == 2:  # duplicate token
        return "".join(toks[:i] + [toks[i]] + toks[i:])
    if kind == 3:  # swap with random other token
        j = rng.randrange(len(toks))
        toks[i], toks[j] = toks[j], toks[i]
        return "".join(toks)
    if kind == 4:  # replace with random token from pool
        pool = [
            "(",
            ")",
            "{",
            "}",
            "[",
            "]",
            ";",
            ",",
            ".",
            ":",
            "=",
            "..",
            "^",
            "&",
            "?",
            "!",
            "fn",
            "struct",
            "union",
            "enum",
            "match",
            "if",
            "else",
            "return",
            "break",
            "continue",
            "loop",
            "for",
            "while",
            "defer",
            "errdefer",
            "const",
            "let",
            "let mut",
            "comptime",
            "pub",
            "type",
            "void",
            "noreturn",
            "i32",
            "u8",
            "bool",
            "true",
            "undefined",
            "0",
            "1",
            "-1",
            '"s"',
            "@as",
            "@sizeOf",
            "@typeInfo",
            "@This",
            "@compileError",
            "mut",
            "dyn",
            "impl",
            "interface",
            "self",
            "...",
            "_",
            "=>",
            "|x|",
            "null",
            "test",
            "import",
            "extern",
            "export",
            '@"😀"',
            '@"caf\\u{E9}"',
            '"你好\\u{1F600}"',
            '"\\xFF"',
            "'é'",
            "'😀'",
            "'\\u{10FFFF}'",
            "'ab'",
            '"\\u{D800}"',
        ]
        toks[i] = " " + rng.choice(pool) + " "
        return "".join(toks)
    if kind == 7:  # rename an identifier to a non-ASCII raw identifier everywhere
        names = sorted({t for t in toks if t.isidentifier() and t.isascii()})
        if names:
            name = rng.choice(names)
            raw = rng.choice(['@"😀"', '@"内部"', '@"e\\u{301}"', '@"👨‍👩‍👧"'])
            return "".join(raw if t == name else t for t in toks)
    if kind == 5:  # delete a span
        j = min(len(toks), i + rng.randrange(1, 12))
        return "".join(toks[:i] + toks[j:])
    # splice a span from elsewhere
    j = rng.randrange(len(toks))
    k = min(len(toks), j + rng.randrange(1, 12))
    return "".join(toks[:i] + toks[j:k] + toks[i:])


def mutated_cases(seed, per_program, limit=None):
    rng = random.Random(seed)
    progs = programs()
    if limit:
        rng.shuffle(progs)
        progs = progs[:limit]
    out = []
    for name, src in progs:
        for k in range(per_program):
            out.append((f"m{seed}_{name}_{k}", mutate(src, rng)))
    return out
