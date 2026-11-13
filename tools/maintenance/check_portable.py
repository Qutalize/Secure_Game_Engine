"""Verify the copyable workspace against its relative-path SHA-256 inventory.

This is an accidental loss/corruption check, not an authenticated release signature.
No directory links are followed. Keys are hashed, never printed or exported.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path('docs/maintenance/transfer-manifest.json')


def files(root):
    for base, dirs, names in os.walk(root, followlinks=False):
        for name in dirs + names:
            path = Path(base) / name
            if getattr(path.lstat(), 'st_file_attributes', 0) & 0x400 or path.is_symlink():
                raise ValueError('Link/reparse point is not portable: ' + str(path))
        for name in names:
            path = Path(base) / name
            relative = path.relative_to(root).as_posix()
            if relative != MANIFEST.as_posix():
                yield relative, path


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--write', action='store_true', help='Explicitly replace the inventory after intentional edits')
    args = parser.parse_args()
    root = args.root.resolve()
    entries = dict(files(root))  # Also rejects links among files added after the inventory.
    if args.write:
        data = {name: {'bytes': path.stat().st_size, 'sha256': digest(path)}
                for name, path in sorted(entries.items())}
        target = root / MANIFEST
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps({'schema': 1, 'files': data}, indent=2), encoding='utf8')
        print(f'Inventoried {len(data)} files, {sum(v["bytes"] for v in data.values()):,} bytes')
        return 0
    data = json.loads((root / MANIFEST).read_text(encoding='utf8'))['files']
    failures = []
    for name, expected in data.items():
        path = entries.get(name)
        if path is None:
            failures.append('missing: ' + name)
        elif path.stat().st_size != expected['bytes'] or digest(path) != expected['sha256']:
            failures.append('changed: ' + name)
    print(f'Checked {len(data)} original files; {len(set(entries) - set(data))} additional files; {len(failures)} failures')
    for failure in failures:
        print(failure)
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
