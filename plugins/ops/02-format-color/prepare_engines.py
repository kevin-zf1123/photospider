#!/usr/bin/env python3
"""Apply identified, build-local integration changes to pinned engine sources."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil

def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("Pinned engine source changed: " + old[:100])
    return text.replace(old, new)

def loops(text):
    # Preserve offsets while hiding comments, literals and preprocessor lines.
    token = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|^[ \t]*\#(?:[^\n]*\\\n)*[^\n]*', re.S | re.M)
    masked = token.sub(lambda m: " " * len(m[0]), text)
    changes = []
    for found in re.finditer(r'\b(for|while)\s*\(', masked):
        begin = found.end()
        depth, end = 1, begin
        semicolons = []
        while depth:
            if masked[end] == '(':
                depth += 1
            elif masked[end] == ')':
                depth -= 1
            elif depth == 1 and masked[end] == ';':
                semicolons.append(end)
            end += 1
        if found[1] == 'for':
            if len(semicolons) != 2:
                raise RuntimeError("Unexpected C for loop")
            first, last = semicolons[0]+1, semicolons[1]
        else:
            first, last = begin, end-1
        condition = text[first:last].strip() or '1'
        changes.append((first,last,' ps_engine_tick(), ('+condition+') '))
    for first,last,value in sorted(changes,reverse=True):
        text=text[:first]+value+text[last:]
    return text

def lcms(source, output):
    stamp = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    marker = output / '.photospider-integration'
    if marker.exists() and marker.read_text() == stamp:
        return
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    for directory in ('include','src'):
        shutil.copytree(source/directory,output/directory)
    path = output/'src/cmsgmt.c'
    s=path.read_text()
    s=replace(s,'nGridpoints = _cmsReasonableGridpointsByColorspace(ColorSpace, cmsFLAGS_HIGHRESPRECALC);',
              '/* Grid is selected below from the source signature. */')
    s=replace(s,'nInputChannels  = cmsChannelsOfColorSpace(InputColorSpace);',
              'nInputChannels  = cmsChannelsOfColorSpace(InputColorSpace);\n'
              '    nGridpoints = _cmsReasonableGridpointsByColorspace(InputColorSpace, cmsFLAGS_HIGHRESPRECALC);')
    s=replace(s,'Gamut = cmsPipelineAlloc(ContextID, 3, 1);',
              'Gamut = cmsPipelineAlloc(ContextID, nInputChannels, 1);')
    s=replace(s,'cmsStageAllocCLut16bit(ContextID, nGridpoints, nChannels, 1, NULL)',
              'cmsStageAllocCLut16bit(ContextID, nGridpoints, nInputChannels, 1, NULL)')
    s=replace(s,'cmsStageSampleCLut16bit(CLUT, GamutSampler, (void*) &Chain, 0);',
              'if (!cmsStageSampleCLut16bit(CLUT, GamutSampler, (void*) &Chain, 0)) {\n'
              '                    cmsPipelineFree(Gamut); Gamut = NULL;\n'
              '                    ps_engine_error("incomplete gamut table");\n'
              '                }')
    for variable, call in [('LabIn1','cmsDoTransform(t -> hInput, In, &LabIn1, 1);'),
                           ('LabOut1','cmsDoTransform(t -> hReverse, Proof, &LabOut1, 1);'),
                           ('LabOut2','cmsDoTransform(t -> hReverse, Proof2, &LabOut2, 1);')]:
        s=replace(s,call,call+'\n    if (!isfinite('+variable+'.L) || !isfinite('+variable+'.a) || !isfinite('+variable+'.b)) { ps_engine_error("nonfinite gamut sampler"); return 0; }')
    s=replace(s,'dE2 = cmsDeltaE(&LabIn2, &LabOut2);',
              'dE2 = cmsDeltaE(&LabIn2, &LabOut2);\n'
              '    if (!isfinite(dE1) || !isfinite(dE2)) { ps_engine_error("nonfinite gamut distance"); return 0; }')
    s=replace(s,'if (ErrorRatio > t->Threshold)',
              'if (!isfinite(ErrorRatio) || ErrorRatio > 65535.0 + t->Threshold) { ps_engine_error("invalid gamut ratio"); return 0; }\n'
              '                if (ErrorRatio > t->Threshold)')
    s=replace(s,'// if dE1 is small and dE2 is small, value is likely to be in gamut',
              'if (dE1 > 65535.0 + t->Threshold) { ps_engine_error("invalid gamut distance"); return 0; }\n'
              '    // if dE1 is small and dE2 is small, value is likely to be in gamut')
    path.write_text(s)
    path=output/'src/cmsio1.c'
    s=path.read_text().replace('cmsReadTag(', 'ps_engine_icc_read_tag(')
    for function,route in [('_cmsReadInputLUT','input'),('_cmsReadOutputLUT','output'),('_cmsReadDevicelinkLUT','forward')]:
        signature='cmsPipeline* CMSEXPORT '+function+'(cmsHPROFILE hProfile, cmsUInt32Number Intent)\n{'
        s=replace(s,signature,signature+'\n    ps_engine_icc_route(hProfile, \"'+route+'\", Intent);')
    for function in ('BuildGrayInputMatrixPipeline','BuildRGBInputMatrixShaper','BuildGrayOutputPipeline','BuildRGBOutputMatrixShaper'):
        if function+'(cmsHPROFILE hProfile)\n{' in s:
            signature=function+'(cmsHPROFILE hProfile)\n{'
            s=replace(s,signature,signature+'\n    ps_engine_icc_route(hProfile, \"'+function+'\", 0xffffffffU);')
    path.write_text(s)
    for path in (output/'src').glob('*.c'):
        # Context registry loops hold a process-global mutex; never jump there.
        if path.name != 'cmsplugin.c':
            path.write_text(loops(path.read_text()))
    marker.write_text(stamp)
    print('Prepared LCMS 2.19.1 integration: source-domain gamut grid and checked work loops.')

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--lcms-source',type=Path,required=True)
    parser.add_argument('--lcms-output',type=Path,required=True)
    args=parser.parse_args()
    lcms(args.lcms_source,args.lcms_output)
