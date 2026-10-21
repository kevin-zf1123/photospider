#!/usr/bin/env python3
"""Give the private static C++ runtime the same admitted allocation symbols."""
import argparse
import hashlib
from pathlib import Path
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--archive',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--objcopy',required=True)
p.add_argument('--apple',action='store_true')
p.add_argument('--abi',action='store_true')
a=p.parse_args()
mapping={'malloc':'ps_engine_malloc','calloc':'ps_engine_calloc','realloc':'ps_engine_realloc',
         'free':'ps_engine_free','strdup':'ps_engine_strdup','posix_memalign':'ps_engine_posix_memalign'}
for base in ('_Znwm','_Znam'):
    mapping[base]='ps_engine_new'
    mapping[base+'RKSt9nothrow_t']='ps_engine_new_nothrow'
    mapping[base+'St11align_val_t']='ps_engine_new_aligned'
    mapping[base+'St11align_val_tRKSt9nothrow_t']='ps_engine_new_aligned_nothrow'
for base in ('_ZdlPv','_ZdaPv'):
    mapping[base]='ps_engine_delete'
    mapping[base+'m']='ps_engine_delete_sized'
    mapping[base+'St11align_val_t']='ps_engine_delete_sized'
    mapping[base+'mSt11align_val_t']='ps_engine_delete_sized_aligned'
    mapping[base+'RKSt9nothrow_t']='ps_engine_delete_nothrow'
if a.abi:
    # Exception control objects remain allocatable to report exhaustion.
    # The private stdexcept destructor releases admitted message strings.
    mapping={'free':'ps_engine_free'}
prefix='_' if a.apple else ''
digest=hashlib.sha256(a.archive.read_bytes()+Path(__file__).read_bytes()+bytes([a.abi])).hexdigest()
stamp=a.output.with_suffix('.sha256')
if not a.output.exists() or not stamp.exists() or stamp.read_text()!=digest:
    a.output.parent.mkdir(parents=True,exist_ok=True)
    args=[a.objcopy]
    for old,new in mapping.items():args.extend(['--redefine-sym',prefix+old+'='+prefix+new])
    subprocess.run(args+[str(a.archive),str(a.output)],check=True)
    stamp.write_text(digest)
print(digest)
