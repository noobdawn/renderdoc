# -*- coding: utf-8 -*-
"""
RenderDoc -> NoobDawn mass rename (content + paths), signature hiding.
Replayable version for tracking upstream: safe to re-run after every
upstream merge, because it never modifies itself.

Content rules (applied in order):
  RenderDoc  -> NoobDawn
  RENDERDOC  -> NOOBDAWN
  Renderdoc  -> Noobdawn
  renderdoc  -> noobdawn
  RDC -> NBD   only when NOT preceded by [A-Z]  (protects WORDCHARS, DWORDCount, PBUFFERDC...)
  rdc -> nbd   only when NOT preceded by [a-z]  (protects hardcoded, wordcode, iswordc...)
  origrdc -> orignbd (special case missed by the rule above)

Exclusions:
  - this script itself (never content-replaced, never path-renamed)
  - DOC_EXTS: documentation files are skipped for CONTENT replacement
    (not compiled into binaries, irrelevant for signatures, and keeping
    them upstream-verbatim reduces future merge friction). Their paths
    are still renamed if the filename/dir contains a rename token.
  - BINARY_EXTS: binaries are skipped for CONTENT replacement because
    renderdoc->noobdawn changes byte length and can corrupt PE/font
    structures. Paths are still renamed.
  - SKIP_DIRS: VCS metadata, IDE cache and build output directories.
"""
import os, re, sys

# This script lives in <repo>/upstream_sync/. The rename target is the whole
# repository, so ROOT defaults to the parent of this script's directory.
# An explicit repo root may also be passed as the first CLI argument.
SELF = os.path.abspath(__file__)
if len(sys.argv) > 1:
    ROOT = os.path.abspath(sys.argv[1])
else:
    ROOT = os.path.dirname(os.path.dirname(SELF))

SKIP_DIRS = {'.git', '.vs', 'Development', 'Release', 'Win32', 'x64', 'dist', 'build'}

DOC_EXTS = {'.md', '.rst', '.txt'}
BINARY_EXTS = {'.exe', '.dll', '.lib', '.pyd', '.so', '.a', '.dylib',
               '.ttf', '.otf', '.png', '.jpg', '.jpeg', '.gif', '.ico',
               '.pdf', '.zip', '.dxbc'}

def replace_text(s):
    s = s.replace('RenderDoc', 'NoobDawn')
    s = s.replace('RENDERDOC', 'NOOBDAWN')
    s = s.replace('Renderdoc', 'Noobdawn')
    s = s.replace('renderdoc', 'noobdawn')
    s = re.sub(r'(?<![A-Z])RDC', 'NBD', s)
    s = re.sub(r'(?<![a-z])rdc', 'nbd', s)
    s = s.replace('origrdc', 'orignbd')
    return s

def replace_bytes(b):
    b = b.replace(b'RenderDoc', b'NoobDawn')
    b = b.replace(b'RENDERDOC', b'NOOBDAWN')
    b = b.replace(b'Renderdoc', b'Noobdawn')
    b = b.replace(b'renderdoc', b'noobdawn')
    b = re.sub(rb'(?<![A-Z])RDC', b'NBD', b)
    b = re.sub(rb'(?<![a-z])rdc', b'nbd', b)
    b = b.replace(b'origrdc', b'orignbd')
    return b

def process_file(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in DOC_EXTS:
        return 'doc-skip'
    if ext in BINARY_EXTS:
        return 'binary-ext-skip'
    data = open(path, 'rb').read()
    if not data:
        return 'empty'
    # UTF-16 with BOM
    if data[:2] in (b'\xff\xfe', b'\xfe\xff'):
        text = data.decode('utf-16')
        new = replace_text(text)
        if new != text:
            open(path, 'wb').write(new.encode('utf-16'))
            return 'utf16-bom'
        return 'utf16-unchanged'
    # possible UTF-16 without BOM
    if b'\x00' in data[:4096]:
        try:
            text = data.decode('utf-16-le')
            if text.encode('utf-16-le') == data:
                new = replace_text(text)
                if new != text:
                    open(path, 'wb').write(new.encode('utf-16-le'))
                    return 'utf16-nobom'
                return 'utf16-unchanged'
        except (UnicodeDecodeError, UnicodeEncodeError):
            pass
        return 'binary-skip'
    new = replace_bytes(data)
    if new != data:
        open(path, 'wb').write(new)
        return 'changed'
    return 'unchanged'

def main():
    stats = {}
    all_paths = []
    for root, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            p = os.path.join(root, f)
            if os.path.abspath(p) == SELF:
                continue  # never touch this script, content or path
            all_paths.append(p)
            r = process_file(p)
            stats[r] = stats.get(r, 0) + 1
        for d in dirs:
            all_paths.append(os.path.join(root, d))

    # rename paths, deepest first so children move before parents
    renamed = 0
    for p in sorted(all_paths, key=lambda x: x.count(os.sep), reverse=True):
        if not os.path.exists(p):
            continue
        base = os.path.basename(p)
        newbase = replace_text(base)
        if newbase != base:
            newp = os.path.join(os.path.dirname(p), newbase)
            if os.path.exists(newp):
                print('CONFLICT:', p, '->', newp)
                continue
            os.rename(p, newp)
            renamed += 1

    print('content stats:', stats)
    print('paths renamed:', renamed)

if __name__ == '__main__':
    main()
