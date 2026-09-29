"""Runs ghoti on generated programs and keeps the ones that crash the compiler.

A case passes when ghoti exits 0 or 14 (a reported compile error) with no crash marker in its
output. Passing cases are deleted; failing ones stay in the cases directory for reproduction.
"""
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GHOTI = os.environ.get('GHOTI', os.path.join(ROOT, 'zig-out', 'bin', 'ghoti.exe' if os.name == 'nt' else 'ghoti'))
CASES = os.environ.get('GHOTI_FUZZ_CASES', os.path.join(tempfile.gettempdir(), 'ghoti-fuzz'))
os.makedirs(CASES, exist_ok=True)

EXPECTED = {0, 14}
CRASH = re.compile(r'Assertion failed|\): [A-Za-z]:.*\.(?:cc|hh):\d+:\d+|unreachable reached|LLVM ERROR|'
                   r'Broken module|Stack dump|libc\+\+abi|panic:|Instruction does not dominate|'
                   r'Referring to an instruction in another function|must have .* type \(')
ANSI = re.compile(r'\x1b\[[0-9;]*m')


def run_one(name, src, cmd='build-obj', extra=(), timeout=60):
    if isinstance(src, str):
        src = src.encode('utf-8', 'surrogateescape')
    path = os.path.join(CASES, name + '.gh')
    with open(path, 'wb') as f:
        f.write(src)
    out = os.path.join(CASES, name + '.o')
    args = [GHOTI, cmd, path] + list(extra)
    if cmd in ('build-obj', 'build-exe'):
        args += ['-o', out]
    try:
        p = subprocess.run(args, capture_output=True, timeout=timeout, cwd=CASES)
        code = p.returncode
        text = ANSI.sub('', (p.stdout + p.stderr).decode('utf-8', 'replace'))
    except subprocess.TimeoutExpired:
        code, text = 'TIMEOUT', ''
    for leftover in [out] + ([] if classify(code, text) else [path]):
        try:
            os.remove(leftover)
        except OSError:
            pass
    return name, code, text


def classify(code, text):
    """True when the run is a compiler bug rather than a clean success or diagnostic."""
    if code not in EXPECTED:
        return True
    if CRASH.search(text):
        return True
    return code == 14 and 'error' not in text.lower()


def signature(code, text):
    """Groups failures by their crash site, with numbers blanked out."""
    m = CRASH.search(text)
    if m:
        line = text[m.start():].splitlines()[0]
        return re.sub(r'[0-9]+', 'N', line)[:160]
    return 'exit=%s %s' % (code, (text.strip().splitlines() or [''])[-1][:100])


def run_many(cases, cmd='build-obj', jobs=8, extra=()):
    results = []
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = [ex.submit(run_one, n, s, cmd, extra) for n, s in cases]
        for f in cf.as_completed(futs):
            n, c, t = f.result()
            if classify(c, t):
                results.append((n, c, t))
    return results


def report(groups):
    for sig, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print('#### %d x %s' % (len(items), sig))
        for n, c in items[:4]:
            print('   ', os.path.join(CASES, n + '.gh'), c)


if __name__ == '__main__':
    # python harness.py <module with cases()> [command]
    import importlib
    mod = importlib.import_module(sys.argv[1])
    cmd = sys.argv[2] if len(sys.argv) > 2 else 'build-obj'
    cases = mod.cases()
    groups = {}
    for n, c, t in sorted(run_many(cases, cmd)):
        groups.setdefault(signature(c, t), []).append((n, c))
    report(groups)
    print('%d bad groups of %d cases' % (len(groups), len(cases)))
