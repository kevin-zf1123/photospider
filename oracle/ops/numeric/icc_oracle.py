#!/usr/bin/env python3
"""Independent generated ICC structures and hashlib identities for public import.

Run: python3 icc_oracle.py <photospider_numeric_icc>
Fixtures describe synthetic structures, not real printing conditions.
"""

import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile


def u32(n):
    return struct.pack('>I', n)


def mluc(text='Synthetic ICC', records=1, stride=12):
    raw = text.encode('utf-16-be')
    return (b'mluc' + bytes(4) + u32(records) + u32(stride)
            + (b'enUS' + u32(len(raw)) + u32(16 + records*stride)
               + bytes(stride-12))*records + raw)


def lut(i, o, wide=True):
    matrix = b''.join(u32(65536 if j % 4 == 0 else 0) for j in range(9))
    header = (b'mft2' if wide else b'mft1') + bytes(4) + bytes([i, o, 2, 0]) + matrix
    return header + (b'\0\2\0\2' + bytes(2*(i*2 + 2**i*o + o*2)) if wide
                     else bytes(i*256 + 2**i*o + o*256))


def multi(i, o, forward, shared=False, parametric=False, matrix=False):
    result = bytearray((b'mAB ' if forward else b'mBA ') + bytes(4) + bytes([i, o, 0, 0]) + bytes(20))

    def stage(at, payload):
        result[at:at+4] = u32(len(result))
        result.extend(payload)
        result.extend(bytes(-len(result) % 4))

    curve = b'para'+bytes(4)+bytes(4)+u32(65536) if parametric else b'curv'+bytes(8)
    # Shared A/B/M prefix is legal even with different group cardinalities.
    if shared:
        at = len(result)
        stage(12, curve*max(i, o))
        result[28:32] = u32(at)
        if matrix:
            result[20:24] = u32(at)
    else:
        stage(12, curve*(o if forward else i))
        stage(28, curve*(i if forward else o))
        if matrix:
            stage(20, curve*(o if forward else i))
    if matrix:
        stage(16, b''.join(u32(65536 if j in (0, 4, 8) else 0) for j in range(12)))
    stage(24, bytes([2]*i + [0]*(16-i)) + b'\1\0\0\0' + bytes(2**i*o))
    return bytes(result)


def profile(major=4, minor=4, use_id=False, legacy_id=False, wide=True,
            use_multi=False, shared=False, matrix=False, parametric=False,
            multilingual=None):
    if major == 2:
        desc = b'desc'+bytes(4)+u32(2)+b'X\0'+bytes(8+70)
        copyright_ = b'text'+bytes(4)+b'X\0'
    else:
        desc = multilingual if multilingual is not None else mluc()
        copyright_ = mluc('Public domain synthetic structure')
    white = b'XYZ '+bytes(4)+u32(0xf6d6)+u32(65536)+u32(0xd32d)

    def transform(i, o, forward):
        return multi(i, o, forward, shared, parametric, matrix) if use_multi else lut(i, o, wide)

    a, b, g = transform(4, 3, True), transform(3, 4, False), transform(3, 1, False)
    tags = [(b'desc', desc), (b'cprt', copyright_), (b'wtpt', white),
            (b'A2B0', a), (b'A2B1', a), (b'A2B2', a),
            (b'B2A0', b), (b'B2A1', b), (b'B2A2', b), (b'gamt', g)]
    result = bytearray(132+12*len(tags))
    result[8:12] = bytes([major, minor << 4, 0, 0])
    result[12:24] = b'prtrCMYKLab '
    result[24:36] = struct.pack('>6H', 2026, 9, 20, 1, 2, 3)
    result[36:40] = b'acsp'
    result[44:48] = u32(0x12340001)  # Vendor bits plus embedded flag.
    result[56:64] = struct.pack('>Q', 0x1234000000000001)
    result[68:80] = white[8:20]
    result[128:132] = u32(len(tags))
    offsets = {}
    for i, (key, payload) in enumerate(tags):
        if payload not in offsets:
            offsets[payload] = len(result)
            result.extend(payload)
            result.extend(bytes(-len(result) % 4))
        result[132+12*i:144+12*i] = key + u32(offsets[payload]) + u32(len(payload))
    result[0:4] = u32(len(result))
    if use_id:
        assert major == 4
        masked = bytearray(result)
        if legacy_id:
            masked[56:64] = bytes(8)
        else:
            masked[44:48] = bytes(4)
        masked[64:68] = bytes(4)
        masked[84:100] = bytes(16)
        result[84:100] = hashlib.md5(masked).digest()
    return bytes(result)


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    cases = []
    for major in (2, 4):
        for minor in range(5):
            for wide in (False, True):
                cases.append((f'v{major}.{minor} mft{2 if wide else 1}', profile(major, minor, wide=wide), True))
    for minor in range(5):
        cases.append((f'v4.{minor} MD5', profile(minor=minor, use_id=True), True))
    cases.append(('v4.0 historic MD5', profile(minor=0, use_id=True, legacy_id=True), True))
    for shared in (False, True):
        for matrix in (False, True):
            for parametric in (False, True):
                cases.append((f'multi shared={shared} matrix={matrix} para={parametric}',
                              profile(use_multi=True, shared=shared, matrix=matrix, parametric=parametric, use_id=True), True))
    cases.append(('mluc expanded records/share', profile(multilingual=mluc(records=3, stride=16)), True))
    # Surrogate pairs cross every 1024-byte boundary of the scanner.
    text = 'x'*511 + '\U0001f600' + ('x'*510 + '\U0001f600')*3
    cases.append(('mluc surrogate boundaries/shared', profile(multilingual=mluc(text, records=3)), True))
    base = profile()
    edits = [(0, u32(len(base)-4)), (8, bytes([5])), (10, b'\1'), (12, b'link'),
             (16, b'RGB '), (20, b'GRAY'), (26, b'\0\15'), (28, b'\0\40'),
             (36, b'bad!'), (44, u32(4)), (60, u32(16)), (64, u32(4)),
             (68, u32(65536)), (100, b'\1'), (128, u32(0xffffffff)),
             (132, b'cprt'), (136, u32(128)), (136, u32(253)),
             (140, u32(7)), (140, u32(0xffffffff)), (132+12*3, b'nope')]
    for offset, replacement in edits:
        bad = bytearray(base)
        bad[offset:offset+len(replacement)] = replacement
        cases.append((f'header/directory mutation {offset} {replacement.hex()}', bytes(bad), False))
    signed = bytearray(profile(use_id=True))
    signed[-1] ^= 1
    cases.append(('MD5 mismatch', bytes(signed), False))
    for i in range(10):
        offset, size = struct.unpack_from('>II', base, 136+12*i)
        bad = bytearray(base)
        bad[offset+4] = 1
        cases.append((f'tag {i} reserved', bytes(bad), False))
    for length in [0, 4, 127, 128, 131, 200, len(base)-1]:
        cases.append((f'truncate {length}', base[:length], False))
    with tempfile.TemporaryDirectory(prefix='icc-oracle-') as directory:
        path = pathlib.Path(directory)/'synthetic.icc'
        for name, data, valid in cases:
            path.write_bytes(data)
            result = subprocess.run([str(executable), '--inspect', str(path)], check=True,
                                    capture_output=True, text=True).stdout.strip()
            if valid:
                expected = f'OK {len(data)} {hashlib.sha256(data).hexdigest()}'
                assert result == expected, (name, result, expected)
            else:
                assert result.startswith('ERR 1 '), (name, result)
    print(f'ICC independent generated structures/MD5/SHA-256: {len(cases)} cases PASS')


if __name__ == '__main__':
    main()
