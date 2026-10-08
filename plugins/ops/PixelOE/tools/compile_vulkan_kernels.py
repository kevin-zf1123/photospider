"""Wrap scalar entry arguments in reflected Vulkan UBOs and validate SPIR-V."""
import json
import re
import struct
import subprocess

SCALARS = {'float32': ('float', 4), 'uint32': ('uint', 4),
           'int32': ('int', 4), 'uint64': ('uint64_t', 8),
           'int64': ('int64_t', 8)}


def generate(slangc, root, out, module, entry, ep):
    # The pinned sources expose scalar/resource uniforms at entry scope. Vulkan
    # lowers those uniforms to push constants, while the public host ABI uses a
    # UBO. Preserve the body as an ordinary function and call it from a wrapper.
    original = (root / (module + '.slang')).read_text()
    original = re.sub(r'\[shader\("compute"\)\]', '', original)
    original = re.sub(r'\[numthreads\([^]]*\)\]', '', original)
    original = re.sub(r'\buniform\s+', '', original)
    lines = [f'#define {entry} px_impl_{entry}', original, f'#undef {entry}']
    arguments, fields, resources = [], [], []
    for parameter in ep['parameters']:
        name, kind = parameter['name'], parameter['type']
        semantic = parameter.get('semanticName')
        if semantic:
            if semantic != 'SV_DISPATCHTHREADID':
                raise ValueError('Unsupported Vulkan entry semantic: ' + semantic)
            arguments.append('tid')
        elif kind['kind'] == 'resource':
            index = len(resources)
            scalar = kind['resultType']['scalarType']
            ctype, size = SCALARS[scalar]
            writable = int(kind.get('access') == 'readWrite')
            resource_type = ('RW' if writable else '') + 'StructuredBuffer'
            lines.append(f'[[vk::binding({index}, 0)]] {resource_type}<{ctype}> px_{name};')
            arguments.append('px_' + name)
            resources.append((name, index, size, writable, scalar))
        elif kind['kind'] == 'scalar':
            ctype, size = SCALARS[kind['scalarType']]
            fields.append((name, ctype, size, kind['scalarType']))
            arguments.append('px_args.' + name)
        else:
            raise ValueError('Unsupported Vulkan parameter: ' + name)
    constant_index = len(resources)
    if not fields or constant_index > 30:
        raise ValueError('Unsupported Vulkan argument block: ' + entry)
    lines += ['struct PxArguments {']
    lines += [f'  {ctype} {name};' for name, ctype, _, _ in fields]
    lines += ['};', f'[[vk::binding({constant_index}, 0)]] ConstantBuffer<PxArguments> px_args;',
              '[shader("compute")]', f'[numthreads({", ".join(map(str, ep["threadGroupSize"]))})]',
              f'void {entry}(uint3 tid: SV_DispatchThreadID) {{',
              f'  px_impl_{entry}({", ".join(arguments)});', '}']
    wrapper = out / (entry + '.vulkan.slang')
    wrapper.write_text('\n'.join(lines) + '\n')
    binary, reflection = out / (entry + '.spv'), out / (entry + '.vulkan.json')
    command = [str(slangc), str(wrapper), '-entry', entry, '-target', 'spirv',
               '-profile', 'spirv_1_5', '-fp-mode', 'precise', '-fvk-use-entrypoint-name',
               '-o', str(binary), '-reflection-json', str(reflection)]
    for directory in [(root / module).parent, root, root / 'common', root / 'color', root / 'downscale']:
        command += ['-I', str(directory)]
    subprocess.run(command, check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(binary)], check=True)
    layout = json.loads(reflection.read_text())
    reflected = layout['parameters']
    actual_ep, = layout['entryPoints']
    if (actual_ep['name'] != entry or actual_ep['threadGroupSize'] != ep['threadGroupSize'] or
            len(reflected) != len(resources) + 1):
        raise ValueError('Vulkan entry reflection changed: ' + entry)
    records = []
    for resource, (name, index, size, writable, scalar) in zip(reflected, resources):
        binding, kind = resource['binding'], resource['type']
        if (resource['name'] != 'px_' + name or binding['kind'] != 'descriptorTableSlot' or
                binding['index'] != index or binding.get('space', 0) != 0 or
                kind['baseShape'] != 'structuredBuffer' or
                kind['resultType']['scalarType'] != scalar or
                int(kind.get('access') == 'readWrite') != writable):
            raise ValueError('Vulkan resource reflection changed: ' + entry)
        records.append(f'{{"{name}", GpuKind::Buffer, {index}, 0, {size}, {writable}}}')
    block = reflected[-1]
    if (block['name'] != 'px_args' or block['type']['kind'] != 'constantBuffer' or
            block['binding']['kind'] != 'descriptorTableSlot' or
            block['binding']['index'] != constant_index or
            block['binding'].get('space', 0) != 0):
        raise ValueError('Vulkan UBO binding changed: ' + entry)
    uniform = block['type']['elementType']
    if len(uniform['fields']) != len(fields):
        raise ValueError('Vulkan UBO field count changed: ' + entry)
    occupied = set()
    for actual, (name, _, size, scalar) in zip(uniform['fields'], fields):
        binding = actual['binding']
        offset = binding['offset']
        span = set(range(offset, offset + size))
        if (actual['name'] != name or actual['type']['scalarType'] != scalar or
                binding['kind'] != 'uniform' or binding['size'] != size or span & occupied):
            raise ValueError('Vulkan UBO field layout changed: ' + entry)
        occupied |= span
        kind = 'Float' if scalar == 'float32' else 'Integer'
        records.append(f'{{"{name}", GpuKind::{kind}, 0, {offset}, {size}, 0}}')
    size, = (v['value'] for v in uniform['sizes'] if v['kind'] == 'uniform')
    if size > 512 or max(occupied) >= size or len(records) > 64:
        raise ValueError('Vulkan UBO exceeds host bounds: ' + entry)
    data = binary.read_bytes()
    if not data or len(data) % 4 or len(data) > 262144:
        raise ValueError('Vulkan module exceeds public dispatch bounds: ' + entry)
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    capabilities, cursor = set(), 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 65535
        if not count or cursor + count > len(words):
            raise ValueError('Malformed SPIR-V instruction')
        if opcode == 17:
            capabilities.add(words[cursor + 1])
        cursor += count
    if not capabilities <= {1, 11} or 1 not in capabilities:
        raise ValueError(f'Unsupported Vulkan features for {entry}: {capabilities}')
    definitions = [f'static const uint32_t spirv_{entry}[] = {{']
    definitions += ['  ' + ', '.join(f'0x{v:08x}U' for v in words[i:i+6]) + ','
                    for i in range(0, len(words), 6)]
    definitions += ['};', f'static const GpuParameter vk_parameters_{entry}[] = {{' + ',\n'.join(records) + '};']
    record = (f'{{"{entry}", reinterpret_cast<const char*>(spirv_{entry}), sizeof(spirv_{entry}), '
              f'{{{", ".join(map(str, ep["threadGroupSize"]))}}}, {constant_index}, {size}, '
              f'vk_parameters_{entry}, {len(records)}, PS_GPU_BACKEND_VULKAN_V1, PS_GPU_CODE_SPIRV_V1}}')
    return definitions, record
