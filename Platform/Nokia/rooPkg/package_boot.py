#!/usr/bin/env python3
"""Package an existing roo FD: python3 Platform/Nokia/rooPkg/package_boot.py OUTPUT.

Requires make, the aarch64 cross toolchain, gzip and magiskboot in PATH.
--original and --fd override the default input paths.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_ORIGINAL = Path('/mnt/c/Users/Administrator/Desktop/maozi/tmp/u3/boot_ori.img')
DEFAULT_FD = ROOT / 'Build/roo/RELEASE_CLANG38/FV/ROO_UEFI.fd'


def run(*args, cwd=None, stdout=None):
    subprocess.run([str(a) for a in args], cwd=cwd, stdout=stdout, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='output boot image (must differ from original)')
    parser.add_argument('--original', type=Path, default=DEFAULT_ORIGINAL)
    parser.add_argument('--fd', type=Path, default=DEFAULT_FD)
    args = parser.parse_args()
    original = args.original.resolve(strict=True)
    fd = args.fd.resolve(strict=True)
    output = args.output.absolute()
    if output.is_symlink():
        raise ValueError('output must not be a symlink')
    for source in (original, fd):
        if output.resolve() == source or (output.exists() and os.path.samefile(output, source)):
            raise ValueError('output must not refer to an input file')

    run('make', '-C', ROOT / 'BootShim', 'clean')
    run('make', '-C', ROOT / 'BootShim', 'UEFI_BASE=0x50000000', 'UEFI_SIZE=0x00700000')

    # Stage on the output filesystem so the final rename is atomic.
    with tempfile.TemporaryDirectory(prefix='.roo-boot-', dir=output.parent) as temp:
        temp = Path(temp)
        unpack = temp / 'unpack'
        unpack.mkdir()
        original_copy = temp / 'original.img'
        shutil.copyfile(original, original_copy)
        run('magiskboot', 'unpack', original_copy, cwd=unpack)
        payload = temp / 'payload'
        with payload.open('wb') as dest:
            for source in (ROOT / 'BootShim/BootShim.bin', fd):
                with source.open('rb') as src:
                    shutil.copyfileobj(src, dest)
        # Magiskboot must receive the compressed stream as its kernel file.
        with (unpack / 'kernel').open('wb') as kernel:
            run('gzip', '-n', '-9', '-c', payload, stdout=kernel)
        candidate = temp / 'candidate.img'
        run('magiskboot', 'repack', original_copy, candidate, cwd=unpack)
        os.replace(candidate, output)
    print(f'Wrote {output}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
