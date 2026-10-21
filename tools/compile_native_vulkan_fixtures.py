"""Generate the native Vulkan fixtures using the pinned Slang compiler."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--slangc', type=Path, required=True)
p.add_argument('--spirv-val', default='spirv-val')
p.add_argument('--out', type=Path, default=Path(__file__).resolve().parents[1] /
               'tests/fixtures/native_vulkan_spirv.hpp')
a = p.parse_args()
version = subprocess.check_output([str(a.slangc), "-version"], text=True,
                                  stderr=subprocess.STDOUT).strip()
if version != "2026.18.2":
    raise ValueError(f"Expected Slang 2026.18.2, got {version!r}")
fixtures = Path(__file__).resolve().parents[1] / 'tests/fixtures'
lines = ['#pragma once', '#include <cstdint>',
         '// Generated from Vulkan Slang fixtures with Slang 2026.18.2.',
         '// Regenerate: python3 tools/compile_native_vulkan_fixtures.py --slangc <slangc>']
with tempfile.TemporaryDirectory() as temporary:
    for source, entry, name in [
            ('native_vulkan.slang', 'transform', 'kNativeVulkanSpirv'),
            ('native_vulkan.slang', 'fill', 'kNativeVulkanFillSpirv'),
            ('native_vulkan.slang', 'numeric_probe', 'kNativeVulkanNumericProbeSpirv')]:
        output = Path(temporary) / (entry + '.spv')
        subprocess.run([str(a.slangc), str(fixtures / source), '-entry', entry, '-target', 'spirv',
                        '-profile', 'spirv_1_3', '-fvk-use-entrypoint-name',
                        '-floating-point-mode', 'precise', '-o', str(output)], check=True)
        subprocess.run([a.spirv_val, '--target-env', 'vulkan1.2', str(output)], check=True)
        data = output.read_bytes()
        if len(data) % 4:
            raise ValueError('SPIR-V byte count must contain complete words')
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        lines.append(f'inline constexpr std::uint32_t {name}[] = {{')
        lines += ['    ' + ', '.join(f'0x{word:08x}U' for word in words[i:i+6]) + ','
                  for i in range(0, len(words), 6)]
        lines.append('};')
a.out.write_text('\n'.join(lines) + '\n')
