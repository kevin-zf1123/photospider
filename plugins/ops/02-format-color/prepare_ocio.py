#!/usr/bin/env python3
"""Reproducible private OCIO 2.5.2 host integration; never edits fetched sources."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil

def once(text,old,new):
    if text.count(old)!=1:raise RuntimeError('Pinned source changed: '+old[:100])
    return text.replace(old,new)

def mask(text):
    token=re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|^[ \t]*\#(?:[^\n]*\\\n)*[^\n]*',re.S|re.M)
    return token.sub(lambda m:' '*len(m[0]),text)

def body(text,signature,replacement):
    pos=text.index(signature)
    begin=text.index('{',pos)
    hidden=mask(text);depth=1;end=begin+1
    while depth:
        if hidden[end]=='{':depth+=1
        if hidden[end]=='}':depth-=1
        end+=1
    return text[:begin+1]+'\n'+replacement+'\n'+text[end-1:]

def work_loops(text):
    hidden=mask(text);changes=[]
    for match in re.finditer(r'\b(for|while)\s*\(',hidden):
        begin=match.end();depth=1;end=begin;semicolons=[]
        while depth:
            if hidden[end]=='(':depth+=1
            elif hidden[end]==')':depth-=1
            elif depth==1 and hidden[end]==';':semicolons.append(end)
            end+=1
        if match[1]=='for':
            if len(semicolons)!=2:continue
            first,last=semicolons[0]+1,semicolons[1]
        else:first,last=begin,end-1
        condition=text[first:last].strip() or 'true'
        changes.append((first,last,' ps_engine_cpp_tick(1), ('+condition+') '))
    for first,last,value in sorted(changes,reverse=True):text=text[:first]+value+text[last:]
    return text

p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
stamp=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
marker=a.output/'.photospider-integration'
if marker.exists() and marker.read_text()==stamp:raise SystemExit(0)
if a.output.exists():shutil.rmtree(a.output)
shutil.copytree(a.source,a.output)
root=a.output/'src/OpenColorIO'
path=a.output/'share/cmake/utils/CppVersion.cmake'
s=path.read_text()
s=once(s,'set_property(CACHE CMAKE_CXX_STANDARD PROPERTY STRINGS "${SUPPORTED_CXX_STANDARDS}")','# The parent supplies a target-local C++ standard.')
path.write_text(s)
# Allocation failures must propagate through the private API, rather than
# terminate at upstream allocating setters declared noexcept.
for directory in (a.output/'include',root):
    for path in directory.rglob('*'):
        if path.suffix not in ('.h','.cpp'):continue
        s=path.read_text()
        s=re.sub(r'(what\s*\(\s*\)\s*const)\s*noexcept',r'\1 PS_KEEP_NOEXCEPT',s)
        s=re.sub(r'\bnoexcept\b','',s).replace('PS_KEEP_NOEXCEPT','noexcept')
        path.write_text(s)

path=a.output/'CMakeLists.txt';s=path.read_text()
s=once(s,'include(FindExtPackages)','# Dependencies are pinned and supplied by Photospider.')
path.write_text(s)
path=root/'Platform.cpp';s=path.read_text()
s=body(s,'bool Getenv(const char * name, std::string & value)','    (void)name; value.clear(); return false;')
s=body(s,'bool isEnvPresent(const char * name)','    (void)name; return false;')
path.write_text(s)
path=root/'Context.cpp';s=path.read_text()
s=body(s,'void Context::loadEnvironment()','    // Config defaults and explicit overrides are the only environment.')
path.write_text(s)
path=root/'PathUtils.cpp';s=path.read_text()
s=body(s,'std::string GetCwd()','    return "/";')
s=body(s,'std::string GetFastFileHash(const std::string & filename, const Context & context)',
       '    if (!context.getConfigIOProxy()) return {};\n'
       '    return context.getConfigIOProxy()->getFastLutFileHash(filename.c_str());')
path.write_text(s)
path=root/'Caching.h';s=path.read_text()
s=once(s,'return !m_envDisableAllCaches && m_enabled;','return false;')
path.write_text(s)
path=root/'transforms/FileTransform.cpp';s=path.read_text()
s=body(s,'std::unique_ptr<std::istream> getLutData(','''
    (void)mode;
    if (!config.getConfigIOProxy()) throw ExceptionMissingFile("Closed resource lookup requires a proxy");
    class Buffer final : public std::streambuf {
      std::vector<uint8_t> bytes;
    public:
      explicit Buffer(std::vector<uint8_t> input) : bytes(std::move(input)) {
        char* b=reinterpret_cast<char*>(bytes.data()); setg(b,b,b+bytes.size());
      }
      pos_type seekoff(off_type n,std::ios_base::seekdir direction,std::ios_base::openmode) override {
        off_type base=direction==std::ios_base::beg?0:direction==std::ios_base::cur?gptr()-eback():egptr()-eback();
        if(n < -base || n > (egptr()-eback())-base)return pos_type(off_type(-1));
        setg(eback(),eback()+base+n,egptr());return pos_type(base+n);
      }
      pos_type seekpos(pos_type n,std::ios_base::openmode mode) override {
        return seekoff(off_type(n),std::ios_base::beg,mode);
      }
    };
    class Stream final : public std::istream {
      Buffer buffer;
    public:
      explicit Stream(std::vector<uint8_t> bytes) : std::istream(nullptr),buffer(std::move(bytes)) {rdbuf(&buffer);}
    };
    return std::unique_ptr<std::istream>(new Stream(config.getConfigIOProxy()->getLutData(filepath.c_str())));
''')

s=once(s,'    std::string src = fileTransform.getSrc();',
       '    struct FilePolicy { int previous; explicit FilePolicy(const void* p):previous(ps_engine_ocio_file_enter(p)){}\\n'
       '        ~FilePolicy(){ps_engine_ocio_file_leave(previous);} } policy(&fileTransform);\\n'
       '    std::string src = fileTransform.getSrc();'.replace('\\n','\n'))
s=once(s,'            returnFormat = tryFormat;',
       '            ps_engine_ocio_record("file_reader",tryFormat->getName().c_str());\\n'
       '            returnFormat = tryFormat;'.replace('\\n','\n'))
s=once(s,'            returnFormat = altFormat;',
       '            ps_engine_ocio_record("file_reader",altFormat->getName().c_str());\\n'
       '            returnFormat = altFormat;'.replace('\\n','\n'))

path.write_text(s)
path=root/'fileformats/FileFormatUtils.cpp';s=path.read_text()
s=once(s,'fileInterp = valid ? fileInterp : INTERP_DEFAULT;',
       'if (ps_engine_ocio_strict_interpolation() && !valid && fileInterp != INTERP_DEFAULT && fileInterp != INTERP_BEST) throw Exception("Unsupported concrete LUT interpolation");\n'
       '        fileInterp = valid ? fileInterp : INTERP_DEFAULT;')
s=once(s,'LogWarning(oss.str());',
       'if (ps_engine_ocio_strict_interpolation() && interp != INTERP_DEFAULT && interp != INTERP_BEST) throw Exception(oss.str().c_str());\n'
       '    LogWarning(oss.str());')
path.write_text(s)
path=root/'transforms/ColorSpaceTransform.cpp';s=path.read_text()
s=once(s,'auto view = config.getDefaultSceneToDisplayViewTransform();',
       'auto view = config.getDefaultSceneToDisplayViewTransform();\n'
       '        ps_engine_ocio_bridge(view ? view->getName() : "");')

s=once(s,'    if(AreColorSpacesInSameEqualityGroup(srcColorSpace, dstColorSpace))\\n        return;'.replace('\\n','\n'),
       '    ps_engine_ocio_record("space_from",srcColorSpace->getName());\\n'
       '    ps_engine_ocio_record("space_to",dstColorSpace->getName());\\n'
       '    if(srcColorSpace->getReferenceSpaceType()!=dstColorSpace->getReferenceSpaceType()){\\n'
       '        auto bridge=config.getDefaultSceneToDisplayViewTransform();\\n'
       '        ps_engine_ocio_bridge(bridge ? bridge->getName() : "");\\n'
       '        if(!bridge)throw Exception("Missing explicit default reference bridge");\\n'
       '    }\\n'
       '    if(AreColorSpacesInSameEqualityGroup(srcColorSpace, dstColorSpace)){\\n'
       '        ps_engine_ocio_record("space_elision","identity_or_equalitygroup"); return;\\n'
       '    }'.replace('\\n','\n'))
s=once(s,'    if(dataBypass && (dstColorSpace->isData() || srcColorSpace->isData()))\\n        return;'.replace('\\n','\n'),
       '    if(dataBypass && (dstColorSpace->isData() || srcColorSpace->isData())){\\n'
       '        ps_engine_ocio_record("space_elision","data_bypass"); return;\\n'
       '    }'.replace('\\n','\n'))
path.write_text(s)
path=root/'transforms/LookTransform.cpp';s=path.read_text()
s=once(s,'                      options[0]);',
       '                      options[0]);\\n'
       '        std::ostringstream selected; LookParseResult::serialize(selected,options[0]);\\n'
       '        ps_engine_ocio_record("selected_look_candidate",selected.str().c_str());'.replace('\\n','\n'))
s=once(s,'                success = true;',
       '                std::ostringstream selected; LookParseResult::serialize(selected,options[i]);\\n'
       '                ps_engine_ocio_record("selected_look_candidate",selected.str().c_str());\\n'
       '                success = true;'.replace('\\n','\n'))

path.write_text(s)
# Parsing and graph preparation have exception-safe local ownership. These
# hooks are not added to rendering loops; runtime dispatch stays vectorizable.
paths=list((root/'fileformats').rglob('*.cpp'))
paths += [root/name for name in ('OCIOYaml.cpp','Config.cpp','Context.cpp','OpOptimizers.cpp',
          'Processor.cpp','transforms/GroupTransform.cpp','transforms/LookTransform.cpp',
          'ops/lut1d/Lut1DOpData.cpp','ops/lut3d/Lut3DOpData.cpp')]
for path in paths:path.write_text(work_loops(path.read_text()))

def instrument_function(text,signature):
    pos=text.index(signature);begin=text.index('{',pos);hidden=mask(text);end=begin+1;depth=1
    while depth:
        if hidden[end]=='{':depth+=1
        if hidden[end]=='}':depth-=1
        end+=1
    return text[:begin+1]+work_loops(text[begin+1:end-1])+text[end-1:]
path=root/'ops/lut3d/Lut3DOpCPU.cpp';s=path.read_text()
for signature in ('void BaseLut3DRenderer::updateData(', 'void InvLut3DRenderer::RangeTree::initRanges(',
                  'void InvLut3DRenderer::RangeTree::initInds(', 'void InvLut3DRenderer::RangeTree::indsToHash(',
                  'void InvLut3DRenderer::RangeTree::updateChildren(', 'void InvLut3DRenderer::RangeTree::updateRanges(',
                  'void InvLut3DRenderer::RangeTree::initialize(', 'void InvLut3DRenderer::updateData(',
                  'void InvLut3DRenderer::extrapolate3DArray('):
    s=instrument_function(s,signature)
path.write_text(s)

# Only the enclosing private engine is installed, never upstream C++ targets.
for path in a.output.rglob('CMakeLists.txt'):
    s=path.read_text()
    matches=list(re.finditer(r'(?mi)^\s*install\s*\(',s))
    for match in reversed(matches):
        end=match.end();depth=1
        while depth:
            if s[end]=='(':depth+=1
            elif s[end]==')':depth-=1
            end+=1
        s=s[:match.start()]+'\n# Private dependency: install handled by Photospider.\n'+s[end:]
    path.write_text(s)
marker.write_text(stamp)
print('Prepared OCIO 2.5.2 closed-resource, cache-free, admitted private integration.')
