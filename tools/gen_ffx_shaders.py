#!/usr/bin/env python3
"""gen_ffx_shaders.py GLSLC SPIRV_CROSS SDK_DIR OUT_DIR

The FidelityFX SDK's compute passes for FSR 3.1 (third_party/ffx-sdk: the 3.1 upscaler, optical flow, frame
interpolation; docs/fsr3-plan.md), compiled in place of AMD's FidelityFX_SC.exe (a Windows tool) into the
`<pass>_permutations.h` headers AMD's blob accessors (src/backends/shared/blob_accessors) include - the same names and
layout: a permutation key (the options' bits, `index`), an indirection table from key to variant, and each variant's
SPIR-V and resources (names, bindings, counts, sets). The wave64 and 16-bit headers the accessors also include name
the same tables (rvk's device enables neither).

Only the variants rvk's inputs can ask for are compiled (OPTIONS): the rest of the keys point at the variant with the
same varying options. Resource names are the shader variables' (from an unoptimised compile's debug names: the
backend binds by them; a uniform block by its instance name).
"""
import concurrent.futures
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

glslc, spirv_cross, sdk, out = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]), pathlib.Path(sys.argv[4])
GPU = sdk / 'include' / 'FidelityFX' / 'gpu'

# Per component: its defines (AMD's CMakeCompile*Shaders.txt) and the values compiled for each permutation option
# (an option missing here is compiled as 0).
COMPONENTS = {
    'fsr3upscaler': (['-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0',
                      '-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0',
                      '-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1',
                      '-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0',
                      '-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2'],
                     {'FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE': (0, 1),
                      'FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS': (1,),
                      'FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING': (0, 1)}),
    'frameinterpolation': (['-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0',
                            '-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0',
                            '-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1',
                            '-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0',
                            '-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2'],
                           {'FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS': (0, 1),
                            'FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS': (0, 1)}),
    'opticalflow': ([], {'FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT': (0, 1)}),
}

KINDS = ('constantBuffer', 'srvTexture', 'uavTexture', 'srvBuffer', 'uavBuffer', 'sampler', 'rtAccelerationStructure')


def options_of(component):
    """The permutation options the accessor sets, in its order (POPULATE_PERMUTATION_KEY)."""
    src = (sdk / 'src' / 'backends' / 'shared' / 'blob_accessors' / f'ffx_{component}_shaderblobs.cpp').read_text()
    block = src[src.index('#define POPULATE_PERMUTATION_KEY'):]
    block = block[:block.index('\n\n')]
    return [o for o in re.findall(r'key\.(\w+)\s*=', block) if o != 'index']


def variable_names(spv_path):
    """(set, binding) -> the variable's name, from the SPIR-V's OpName / OpDecorate."""
    words = struct.unpack(f'<{spv_path.stat().st_size // 4}I', spv_path.read_bytes())
    names, sets, binds = {}, {}, {}
    i = 5
    while i < len(words):
        op, n = words[i] & 0xFFFF, words[i] >> 16
        if op == 5:                                     # OpName
            raw = struct.pack(f'<{n - 2}I', *words[i + 2:i + n])
            names[words[i + 1]] = raw.split(b'\0', 1)[0].decode()
        elif op == 71 and words[i + 2] in (33, 34):     # OpDecorate Binding / DescriptorSet
            (binds if words[i + 2] == 33 else sets)[words[i + 1]] = words[i + 3]
        i += max(n, 1)
    return {(sets.get(v, 0), b): names.get(v, f'_{v}') for v, b in binds.items()}


def compile_variant(task):
    src, defines, opt, tmp, tag = task
    spv = tmp / f'{tag}{opt}.spv'
    subprocess.run([glslc, '-fshader-stage=comp', '--target-env=vulkan1.2', opt, '-DFFX_GPU=1', '-DFFX_GLSL=1',
                    '-DFFX_HALF=0', *defines, f'-I{GPU}', f'-I{src.parent.name and GPU / src.parent.name}',
                    str(src), '-o', str(spv)], check=True)
    js = spv.with_suffix('.json')
    subprocess.run([spirv_cross, str(spv), '--reflect', '--output', str(js)], check=True)
    return spv, json.loads(js.read_text())


def resources(reflection, names):
    """Each kind's (name, binding, count, set), in binding order."""
    found = {k: [] for k in KINDS}

    def add(kind, r):
        count = r.get('array', [1])[0] or 1
        key = (r.get('set', 0), r['binding'])
        found[kind].append((names.get(key, r['name']), r['binding'], count, r.get('set', 0)))

    for r in reflection.get('ubos', []):
        add('constantBuffer', r)
    for r in reflection.get('separate_images', []) + reflection.get('textures', []):
        add('srvTexture', r)
    for r in reflection.get('images', []):
        add('uavTexture', r)
    for r in reflection.get('ssbos', []):
        add('srvBuffer' if r.get('readonly') else 'uavBuffer', r)
    for r in reflection.get('separate_samplers', []):
        add('sampler', r)
    for r in reflection.get('acceleration_structures', []):
        add('rtAccelerationStructure', r)
    for k in KINDS:
        found[k].sort(key=lambda x: x[1])
    return found


def c_array(ctype, name, values):
    return f'static const {ctype} {name}[] = {{{", ".join(values)}}};'


def main():
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as t, concurrent.futures.ThreadPoolExecutor(8) as pool:
        tmp = pathlib.Path(t)
        for component, (base, chosen) in COMPONENTS.items():
            options = options_of(component)
            varying = [o for o in options if len(chosen.get(o, (0,))) > 1]
            for src in sorted((sdk / 'src' / 'backends' / 'vk' / 'shaders' / component).glob('*.glsl')):
                name = src.stem
                variants = [{}]
                for o in options:
                    variants = [dict(v, **{o: x}) for v in variants for x in chosen.get(o, (0,))]
                tasks = []
                for n, v in enumerate(variants):
                    defines = base + [f'-D{o}={x}' for o, x in v.items()]
                    src_for = src if src.parent.name else src
                    tasks.append((src_for, defines, '-O0', tmp, f'{name}_{n}'))
                    tasks.append((src_for, defines, '-Os', tmp, f'{name}_{n}'))
                done = list(pool.map(compile_variant, tasks))
                lines = ['// Generated by tools/gen_ffx_shaders.py - do not edit.', '#pragma once', '#include <stdint.h>',
                         '#ifndef RVK_FFX_PERMUTATION_INFO', '#define RVK_FFX_PERMUTATION_INFO',
                         'typedef struct RvkFfxPermutationInfo {', '    const uint8_t* blobData;', '    uint32_t blobSize;']
                lines += [f'    uint32_t num{k[0].upper() + k[1:]}s;' for k in
                          ('constantBuffer', 'SRVTexture', 'UAVTexture', 'SRVBuffer', 'UAVBuffer', 'sampler',
                           'RTAccelerationStructure')]
                for k in KINDS:
                    lines += [f'    const char** {k}Names;', f'    const uint32_t* {k}Bindings;',
                              f'    const uint32_t* {k}Counts;', f'    const uint32_t* {k}Spaces;']
                lines += ['} RvkFfxPermutationInfo;', '#endif', '']
                lines.append(f'typedef union {name}_PermutationKey {{')
                lines.append('    struct {')
                lines += [f'        uint32_t {o} : 1;' for o in options]
                lines += ['    };', '    uint32_t index;', f'}} {name}_PermutationKey;', '']
                infos = []
                for n in range(len(variants)):
                    (plain, _), (spv, refl) = done[2 * n], done[2 * n + 1]
                    found = resources(refl, variable_names(plain))
                    tag = f'g_{name}_{n}'
                    data = spv.read_bytes()
                    lines.append(f'static const uint8_t {tag}_data[] = {{')
                    lines += ['    ' + ', '.join(str(b) for b in data[i:i + 24]) + ',' for i in range(0, len(data), 24)]
                    lines.append('};')
                    fields = []
                    for k in KINDS:
                        r = found[k]
                        if r:
                            nm = [(x[0][:-2] if k == 'constantBuffer' and x[0].endswith('_t') else x[0]) for x in r]
                            lines.append(c_array('char*', f'{tag}_{k}_names', [f'"{x}"' for x in nm]).replace(
                                'static const char*', 'static const char*'))
                            lines.append(c_array('uint32_t', f'{tag}_{k}_bindings', [str(x[1]) for x in r]))
                            lines.append(c_array('uint32_t', f'{tag}_{k}_counts', [str(x[2]) for x in r]))
                            lines.append(c_array('uint32_t', f'{tag}_{k}_spaces', [str(x[3]) for x in r]))
                            fields.append((len(r), f'(const char**){tag}_{k}_names', f'{tag}_{k}_bindings',
                                           f'{tag}_{k}_counts', f'{tag}_{k}_spaces'))
                        else:
                            fields.append((0, 'nullptr', 'nullptr', 'nullptr', 'nullptr'))
                    counts = ', '.join(str(f[0]) for f in fields)
                    arrays = ', '.join(', '.join(f[1:]) for f in fields)
                    infos.append(f'    {{{tag}_data, uint32_t(sizeof({tag}_data)), {counts}, {arrays}}},')
                lines.append(f'static const RvkFfxPermutationInfo g_{name}_PermutationInfo[] = {{')
                lines += infos
                lines.append('};')
                # Every key -> the variant with the same varying options (the fixed ones are rvk's choice).
                table = []
                for key in range(1 << len(options)):
                    bits = {o: (key >> i) & 1 for i, o in enumerate(options)}
                    match = next((n for n, v in enumerate(variants) if all(v[o] == bits[o] for o in varying)), 0)
                    table.append(str(match))
                lines.append(c_array('int32_t', f'g_{name}_IndirectionTable', table))
                (out / f'{name}_permutations.h').write_text('\n'.join(lines) + '\n')
                for suffix in ('wave64', '16bit', 'wave64_16bit'):
                    (out / f'{name}_{suffix}_permutations.h').write_text(
                        '// Generated by tools/gen_ffx_shaders.py - do not edit. (rvk: the same variants.)\n'
                        f'#pragma once\n#include "{name}_permutations.h"\n'
                        f'#define g_{name}_{suffix}_IndirectionTable g_{name}_IndirectionTable\n'
                        f'#define g_{name}_{suffix}_PermutationInfo g_{name}_PermutationInfo\n')
                print(f'{name}: {len(variants)} variants')


main()
