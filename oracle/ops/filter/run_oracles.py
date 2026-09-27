#!/usr/bin/env python3
"""Run self-tests, write a report/bit vectors, or replay a vector file."""
from __future__ import annotations
import argparse
import importlib.metadata
import json
import platform
import sys
from pathlib import Path
from collections import Counter
from tests import run_all,freeze,thaw,equal,MODULES

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test',action='store_true',help='explicit self-test mode (default)')
    parser.add_argument('--report',type=Path,default=Path('oracle-report.json'))
    parser.add_argument('--vectors-out',type=Path)
    parser.add_argument('--no-diagnostics',action='store_true')
    parser.add_argument('--replay',type=Path)
    args=parser.parse_args()
    if args.replay:
        vectors=json.loads(args.replay.read_text(encoding='utf-8'))['vectors'];failed=[]
        for vector in vectors:
            try:
                request=thaw(vector['request']);mod,name=request['function'].split('.',1)
                result=getattr(MODULES[mod],name)(*request['args'],**request['kwargs'])
                # Compare encoded floats directly; exact Fraction outputs stay exact.
                if freeze(result,vector['dtype'])!=vector['expected']:raise AssertionError('replay differs')
            except Exception as exc:failed.append({'case_id':vector['case_id'],'error':str(exc)})
        print(json.dumps({'replayed':len(vectors),'failed':failed},ensure_ascii=False,indent=2))
        return bool(failed)
    suite=run_all(not args.no_diagnostics);counts=Counter(c['status'] for c in suite.cases)
    versions={}
    for package in ('mpmath','numpy','scipy'):
        try:versions[package]=importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:versions[package]='not_installed'
    from oracles.bounds import Context
    with Context() as ctx:versions['MPFR']=ctx.version
    report={'schema':'FilterOracleSelfTestReport/v1','scope':'independent Python oracles only; no Photospider runtime executed',
            'python':sys.version,'platform':platform.platform(),'dependencies':versions,
            'counts':dict(counts),'members_with_executed_cases':sorted({m for c in suite.cases if c['status']=='passed' for m in c['members']}),
            'not_tested':['runtime registration','production kernel correctness','Region demand execution','metadata API integration','allocator/work accounting','cancellation in runtime','owner lifetime','Apple Silicon/x86 accelerated error envelopes','SMAA/BM3D/CBM3D native versus pinned third-party comparisons','nonfinite/payload coverage outside special reference paths','asynchronous stopping predicates and concurrent execution','band-lazy runtime carriers'],
            'cases':suite.cases}
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    if args.vectors_out:
        args.vectors_out.parent.mkdir(parents=True,exist_ok=True)
        args.vectors_out.write_text(json.dumps({'schema':'FilterReferenceVectors/v1','note':'2D mathematical fixtures, not public tensor ABI; see proof_level per vector','vectors':suite.vectors},ensure_ascii=False,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps({'counts':dict(counts),'vectors':len(suite.vectors),'report':str(args.report)},ensure_ascii=False,indent=2))
    for case in suite.cases:
        if case['status']=='failed':print(case['case_id'],case['error'],file=sys.stderr)
    return counts['failed']>0

if __name__=='__main__':raise SystemExit(main())
