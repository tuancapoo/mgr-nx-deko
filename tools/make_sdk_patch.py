#!/usr/bin/env python3
"""Regenerates patches/rexglue-sdk-v0.10.0.patch after a change to sdk/.

The patch is `git diff --cached --full-index` of a scratch repository whose one commit is upstream rexglue-sdk v0.10.0
(commit c94f5ebdcb3c9d1a460ca48e04f9758448f8d518, checked out with core.autocrlf = true so the files are stored with
LF) and whose index holds this fork's files. To make that repository again from nothing:

    git clone https://github.com/rexglue/rexglue-sdk.git scratch && cd scratch
    git checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
    rm -rf .git && git init && git config core.autocrlf true && git add -A && git commit -m upstream
    (then run this script with --all once)

  python tools/make_sdk_patch.py <scratch repo> [--all | <sdk-relative file> ...]

Without files: every file under sdk/ that git reports as changed or new in this checkout (against HEAD). --all:
every file of sdk/ this repository tracks (thirdparty/FFmpeg and thirdparty/XenosRecomp have their own patches and
are left out). The given files are copied into the scratch repository and staged there; the patch file is rewritten
and the new totals are printed for patches/README.md. Check afterwards: `git apply --check` of the patch on a clean
checkout of the base.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OWN_PATCHES = ('thirdparty/FFmpeg/', 'thirdparty/XenosRecomp/')


def git(repo, *args):
    return subprocess.run(['git', '-C', repo, *args], check=True, capture_output=True, text=True).stdout


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    scratch = args[0]
    files = [a.replace('\\', '/') for a in args[1:] if not a.startswith('--')]
    if '--all' in args:
        files = [l[len('sdk/'):] for l in git(ROOT, 'ls-files', 'sdk').split('\n') if l.startswith('sdk/')]
    elif not files:
        for line in git(ROOT, 'status', '--porcelain', '--untracked-files=all', '--', 'sdk').split('\n'):
            if len(line) > 3 and line[0] != 'D' and line[1] != 'D':
                path = line[3:].strip().strip('"')
                if path.startswith('sdk/'):
                    files.append(path[len('sdk/'):])
    files = [f for f in files if not f.startswith(OWN_PATCHES)]
    for rel in files:
        dst = os.path.join(scratch, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, 'sdk', rel), dst)
        git(scratch, 'add', '--', rel)
    patch = subprocess.run(['git', '-C', scratch, 'diff', '--cached', '--full-index'], check=True,
                           capture_output=True).stdout
    with open(os.path.join(ROOT, 'patches', 'rexglue-sdk-v0.10.0.patch'), 'wb') as f:
        f.write(patch)
    print('%d files staged; the patch now has%s' % (len(files), git(scratch, 'diff', '--cached', '--shortstat').rstrip()))


if __name__ == '__main__':
    main()
