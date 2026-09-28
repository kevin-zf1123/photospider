"""Export and verify portable Slang CPU source, frozen tables and prelude."""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import shutil
import subprocess

from compile_kernels import ENTRIES


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_identity(root):
    files = sorted((root / 'third_party/shaders').rglob('*.slang'))
    files += [root / 'src/copy.slang', root / 'tools/compile_vulkan_kernels.py', root / 'tools/compile_kernels.py', root / 'tools/compile_gpu_kernels.py', root / 'tools/generate_tables.py']
    return {str(p.relative_to(root)): digest(p) for p in files}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['export', 'verify'])
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--generated', type=Path)
    parser.add_argument('--slang-root', type=Path)
    args = parser.parse_args()
    expected = {name + '.cpp' for entries in ENTRIES.values() for name in entries}
    expected |= {'kernels.cpp', 'gpu_kernels.cpp', 'tables.cpp'}
    manifest = args.bundle / 'CPU_SOURCES.json'
    if args.mode == 'export':
        if not args.generated or not args.slang_root:
            parser.error('export needs generated and slang-root')
        if {p.name for p in args.generated.glob('*.cpp')} != expected:
            raise RuntimeError('generated kernel inventory differs from source')
        args.bundle.mkdir(parents=True, exist_ok=True)
        for name in sorted(expected):
            text = (args.generated / name).read_text()
            lines = text.splitlines(keepends=True)
            if lines and 'slang-cpp-prelude.h' in lines[0]:
                lines[0] = '#include "slang-cpp-prelude.h"\n'
            (args.bundle / name).write_text(''.join(lines))
        prelude = args.bundle / 'prelude'
        prelude.mkdir(exist_ok=True)
        for name in ['slang-cpp-prelude.h', 'slang-cpp-types.h',
                     'slang-cpp-types-core.h', 'slang-cpp-scalar-intrinsics.h']:
            shutil.copyfile(args.slang_root / 'include' / name, prelude / name)
        shutil.copyfile(args.slang_root / 'LICENSE', prelude / 'LICENSE')
        # Preserve the associated full license and third-party notices.
        for name in ['LICENSES', 'third-party-notices']:
            shutil.copytree(args.slang_root / name, prelude / name, dirs_exist_ok=True)
        version = subprocess.check_output([str(args.slang_root / 'bin/slangc'), '-version'],
                                          stderr=subprocess.STDOUT, text=True).strip()
        records = {str(p.relative_to(args.bundle)): digest(p)
                   for p in sorted(args.bundle.rglob('*')) if p.is_file() and p != manifest}
        manifest.write_text(json.dumps(dict(format=1, slang=version,
                                            numpy=importlib.metadata.version('numpy'),
                                            torch=importlib.metadata.version('torch'),
                                            source=source_identity(args.root), files=records),
                                       indent=2) + '\n')
    record = json.loads(manifest.read_text())
    if record['format'] != 1 or record['source'] != source_identity(args.root):
        raise RuntimeError('CPU source bundle does not match shader/generator source')
    if {p.name for p in args.bundle.glob('*.cpp')} != expected:
        raise RuntimeError('CPU source bundle kernel inventory differs')
    actual = {str(p.relative_to(args.bundle)): digest(p)
              for p in sorted(args.bundle.rglob('*')) if p.is_file() and p != manifest}
    if record['files'] != actual:
        raise RuntimeError('CPU source bundle integrity mismatch')
    print('Verified Slang', record['slang'], 'CPU kernels and frozen tables')


if __name__ == '__main__':
    main()
