"""Generate portable MSL strings and target-reflected argument records."""
import argparse
import json
import re
import subprocess
from pathlib import Path

from compile_kernels import ENTRIES
from compile_vulkan_kernels import generate as generate_vulkan

GPU_ENTRIES = {**ENTRIES, 'downscale/kcentroid_atomic': ['kc_iter_max'], '../../src/copy': ['copy_words']}


def generate(slangc, root, out):
    version = subprocess.check_output([str(slangc), '-version'], text=True,
                                      stderr=subprocess.STDOUT).strip()
    if version != '2026.18.2':
        raise ValueError(f'Expected Slang 2026.18.2, got {version!r}')
    out.mkdir(parents=True, exist_ok=True)
    definitions, records = [], []
    for module, entries in GPU_ENTRIES.items():
        for entry in entries:
            shader = out / (entry + '.metal')
            reflection = out / (entry + '.metal.json')
            command = [str(slangc), str(root / (module + '.slang')),
                       '-entry', entry, '-target', 'metal', '-fp-mode', 'precise',
                       '-o', str(shader), '-reflection-json', str(reflection)]
            for directory in [root, root / 'common', root / 'color', root / 'downscale']:
                command += ['-I', str(directory)]
            subprocess.run(command, check=True)
            layout = json.loads(reflection.read_text())
            ep, = layout['entryPoints']
            if layout['parameters'] or ep['name'] != entry or ep['stage'] != 'compute':
                raise RuntimeError('unsupported GPU entry layout: ' + entry)
            group = ep['threadGroupSize']
            if len(group) != 3 or any(x <= 0 for x in group):
                raise RuntimeError('invalid GPU group: ' + entry)
            scope = ep['scope']
            if scope['kind'] != 'constantBuffer' or scope['binding']['kind'] != 'constantBuffer':
                raise RuntimeError('unsupported GPU constant container: ' + entry)
            constant_index = scope['binding']['index']
            bindings, parameters, constant_size = {constant_index}, [], 0
            occupied = set()
            # Absolute entry parameters include the constant-container offset.
            for param in ep['parameters']:
                if param.get('semanticName') == 'SV_DISPATCHTHREADID':
                    continue
                name, kind, binding = param['name'], param['type'], param['binding']
                if kind['kind'] == 'resource':
                    scalar = kind['resultType']
                    if kind['baseShape'] != 'structuredBuffer' or scalar['kind'] != 'scalar':
                        raise RuntimeError('unsupported GPU resource: ' + name)
                    stride = {'float32': 4, 'uint32': 4, 'int32': 4,
                              'uint64': 8, 'int64': 8}[scalar['scalarType']]
                    index = binding['index']
                    if binding['kind'] != 'constantBuffer' or index in bindings or not 0 <= index <= 30:
                        raise RuntimeError('invalid absolute resource index: ' + name)
                    bindings.add(index)
                    writable = int(kind.get('access') == 'readWrite')
                    parameters.append(f'{{"{name}", GpuKind::Buffer, {index}, 0, {stride}, {writable}}}')
                elif kind['kind'] == 'scalar':
                    scalar = kind['scalarType']
                    size = {'float32': 4, 'uint32': 4, 'int32': 4,
                            'uint64': 8, 'int64': 8}[scalar]
                    offset = binding['offset']
                    span = set(range(offset, offset + size))
                    if binding['kind'] != 'uniform' or binding['size'] != size or span & occupied:
                        raise RuntimeError('invalid scalar layout: ' + name)
                    occupied |= span
                    constant_size = max(constant_size, offset + size)
                    field = 'Float' if scalar == 'float32' else 'Integer'
                    parameters.append(f'{{"{name}", GpuKind::{field}, 0, {offset}, {size}, 0}}')
                else:
                    raise RuntimeError('unsupported GPU parameter: ' + name)
            if constant_size > 512 or len(parameters) > 64:
                raise RuntimeError('GPU parameter capacity: ' + entry)
            source = re.sub(r'^\s*#line[^\n]*\n', '', shader.read_text(), flags=re.M)
            shader.write_text(source)
            definitions.append(f'static const char source_{entry}[] = R"PXMSL({source})PXMSL";')
            definitions.append(f'static const GpuParameter parameters_{entry}[] = {{' + ',\n'.join(parameters) + '};')
            records.append(f'{{"{entry}", source_{entry}, sizeof(source_{entry})-1, '
                           f'{{{", ".join(map(str, group))}}}, {constant_index}, {constant_size}, '
                           f'parameters_{entry}, {len(parameters)}, PS_GPU_BACKEND_METAL_V11, PS_GPU_CODE_MSL_V11}}')
            vk_definitions, vk_record = generate_vulkan(slangc, root, out, module, entry, ep)
            definitions.extend(vk_definitions)
            records.append(vk_record)
    text = '#include "gpu_runtime.hpp"\nnamespace px {\nnamespace {\n'
    text += '\n'.join(definitions) + '\n}\n'
    text += 'const std::vector<GpuKernel>& gpu_kernels() {\nstatic const std::vector<GpuKernel> kernels = {\n'
    text += ',\n'.join(records) + '\n}; return kernels; }\n}\n'
    (out / 'gpu_kernels.cpp').write_text(text)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--slangc', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    generate(args.slangc, args.root, args.out)
