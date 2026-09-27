#!/usr/bin/env python3
"""Reproducible oracle tests and a small JSONL candidate-comparison protocol.

This is NOT a Photospider API adapter. No unimplemented registry key is invoked.
Only exact/analytic finite reference operations are accepted in this protocol;
Measured diagnostics are separate, so they cannot silently certify strict output.
"""
from __future__ import annotations
import argparse
from datetime import datetime,timezone
from fractions import Fraction as F
import io
import importlib.metadata
import json
import math
from pathlib import Path
import platform
import sys
import time
import unittest
import exact as e
import geometry as g
import rng
import resources

HERE=Path(__file__).resolve().parent
OPERATIONS=('coordinate','rectangle_coverage','rectangle_sdf','disk_sdf','sqrt','spread',
            'linear_gradient','radial_gradient','numeric_lookup','linear_rgba_mix','bilinear',
            'barycentric','bezier','polygon_coverage','region_sdf','boolean_regions',
            'philox','uniform','perlin2002','gradient2d','fractal','rank_lookup')
# Conservatively compare copy-capable helpers exactly; never relax selected endpoints.
ACCELERATED=set(OPERATIONS)-{'philox','uniform','boolean_regions','numeric_lookup','linear_rgba_mix','bezier','spread'}


def decode(value):
    if isinstance(value,dict):
        if set(value)=={'bits','dtype'}:
            return e.from_bits(int(value['bits'],16),value['dtype'])
        if set(value)=={'rational'}:
            return F(value['rational'])
        return {k:decode(v) for k,v in value.items()}
    if isinstance(value,list):return [decode(v) for v in value]
    if isinstance(value,float) and not math.isfinite(value):raise ValueError('finite JSON numbers required')
    return value


def fp(value,dtype='float64'):
    # Exact copy sign for actual binary zeros; rational zero is +0 by default.
    neg=isinstance(value,float) and value==0 and math.copysign(1,value)<0
    return {'dtype':dtype,'bits':f'{e.round_bits(F(value),dtype,neg):0{32//4 if dtype=="float32" else 64//4}x}'}


def rational_tree(value):
    if isinstance(value,F):return {'rational':f'{value.numerator}/{value.denominator}'}
    if isinstance(value,(list,tuple)):return [rational_tree(v) for v in value]
    if isinstance(value,dict):return {k:rational_tree(v) for k,v in value.items()}
    return value


CANDIDATE_RNG_OPS={'uniform','gradient2d','fractal'}

def evaluate(request,allow_candidate_rng=False):
    if not isinstance(request,dict) or not isinstance(request.get('id'),str):raise ValueError('request id string required')
    op=request.get('op');a=decode(request.get('args',{}));dtype=a.pop('dtype','float64')
    if op not in OPERATIONS:raise ValueError(f'unsupported oracle operation: {op}')
    if dtype not in ('float32','float64'):raise ValueError('dtype')
    quality='Exact_finite_reference'
    if op in CANDIDATE_RNG_OPS:
        if not allow_candidate_rng:raise ValueError('RNG packing is not frozen; explicitly enable candidate RNG')
        quality='Candidate_rng_layout_not_production_golden:'+rng.CANDIDATE_LAYOUT
    if op=='coordinate':result=[fp(v,dtype) for v in e.coordinate(**a)]
    elif op=='rectangle_coverage':result=fp(e.rectangle_coverage(**a),dtype)
    elif op=='rectangle_sdf':result=fp(e.rectangle_sdf(**a,dtype=dtype),dtype)
    elif op=='disk_sdf':result=fp(e.disk_sdf(**a,dtype=dtype),dtype)
    elif op=='sqrt':result=fp(e.sqrt_fraction(a['q'],dtype),dtype)
    elif op=='spread':result=fp(e.spread_rounded(**a,dtype=dtype),dtype)
    elif op=='linear_gradient':result=fp(e.linear_coordinate(**a),dtype)
    elif op=='radial_gradient':result=fp(e.radial_coordinate(**a,dtype=dtype),dtype)
    elif op=='numeric_lookup':
        t=F(a['t']);table=a['table'];x=t*(len(table)-1)
        # Selected stop is a true copy, including -0; Fraction interpolation
        # alone cannot carry that bit-level distinction.
        if x.denominator==1 and 0<=x<len(table):values=table[int(x)]
        else:values=e.numeric_lookup(t,table)
        result=[fp(v,dtype) for v in values]
    elif op=='linear_rgba_mix':
        values=e.linear_rgba_mix(**a);result=[fp(v,dtype) for v in values]
        if int(result[3]['bits'],16)==0:
            if values[3]>0 and any(int(v['bits'],16)&((1<<(31 if dtype=='float32' else 63))-1) for v in result[:3]):
                raise ArithmeticError('AssociationUnderflow')
            result=[fp(0,dtype) for _ in range(4)]
    elif op=='bilinear':result=[fp(v,dtype) for v in e.bilinear(**a)]
    elif op=='barycentric':result=[fp(v,dtype) for v in e.barycentric(**a)]
    elif op=='bezier':
        pos=g.rounded_bezier_position(a['controls'],a['t'],dtype)
        der=g.bezier_derivative(a['controls'],a['t']);tan,valid=g.unit_tangent_fixture(der,dtype)
        result={'position':[fp(v,dtype) for v in pos],'derivative':[fp(v,dtype) for v in der],
                'tangent':[fp(v,dtype) for v in tan],'tangent_valid':valid}
    elif op=='polygon_coverage':result=fp(g.polygon_pixel_area(**a),dtype)
    elif op=='region_sdf':result=fp(g.region_sdf(**a,dtype=dtype),dtype)
    elif op=='boolean_regions':
        result=rational_tree(g.boolean_regions(**a));quality='Exact_rational_boundary_before_Float64_publication'
    elif op=='philox':result={'words':[f'{v:016x}' for v in rng.philox4x64(**a)]}
    elif op=='uniform':result=fp(rng.uniform(**a,dtype=dtype),dtype)
    elif op=='perlin2002':result=fp(rng.perlin2002_fraction(**a),dtype)
    elif op=='gradient2d':result=fp(rng.gradient2d_fraction(**a),dtype)
    elif op=='fractal':result=fp(rng.fractal_fraction(**a),dtype)
    elif op=='rank_lookup':result=fp(resources.rank_lookup(**a,dtype=dtype),dtype)
    return {'id':request['id'],'quality':quality,'result':result}


def compare_tree(reference,actual,accelerated=False,path='result'):
    errors=[]
    if isinstance(reference,dict) and set(reference)=={'bits','dtype'}:
        if not isinstance(actual,dict) or set(actual)!=set(reference) or actual['dtype']!=reference['dtype']:
            return [path+': floating type/shape mismatch']
        try:
            rb,ab=int(reference['bits'],16),int(actual['bits'],16)
            ok=(e.accelerated_ok(e.from_bits(rb,reference['dtype']),e.from_bits(ab,reference['dtype']),reference['dtype']) if accelerated else rb==ab)
            return [] if ok else [path+': numeric mismatch']
        except (ValueError,OverflowError,TypeError):return [path+': malformed floating bits']
    if type(reference) is not type(actual):return [path+': type mismatch']
    if isinstance(reference,dict):
        if set(reference)!=set(actual):return [path+': object keys mismatch']
        for k in reference:errors+=compare_tree(reference[k],actual[k],accelerated,path+'.'+k)
    elif isinstance(reference,list):
        if len(reference)!=len(actual):return [path+': list length mismatch']
        for i,(r,a) in enumerate(zip(reference,actual)):errors+=compare_tree(r,a,accelerated,f'{path}[{i}]')
    elif reference!=actual:errors.append(path+': exact/discrete mismatch')
    return errors


def fixture_check(path=HERE/'fixtures.json'):
    data=json.loads(Path(path).read_text(encoding='utf-8'));errors=[]
    for case in data['cases']:
        got=evaluate(case)
        errors.extend(case['id']+': '+x for x in compare_tree(case['expected_result'],got['result']))
    return {'cases':len(data['cases']),'failures':errors,'passed':not errors,
            'provenance':'hand analytic expected bits and external Random123 known-answer vector; not self-generated golden'}


def statistical_report(sample_count=16384):
    """Non-gating deterministic diagnostics. No quality/admission threshold."""
    u=[rng.uniform(i,0,seed=2026) for i in range(sample_count)]
    mean=math.fsum(u)/sample_count;variance=math.fsum((v-mean)**2 for v in u)/sample_count
    n=[]
    for i in range(sample_count):
        a,b=rng.gaussian_inputs(i,0,seed=2026)
        n.append(math.sqrt(-2*math.log(float(a)))*math.cos(2*math.pi*float(b)))
    nm=math.fsum(n)/sample_count;nv=math.fsum((v-nm)**2 for v in n)/sample_count
    report={'quality':'Candidate_rng_Measured_statistics_not_production_golden','seed':2026,'samples':sample_count,
            'uniform_float64':{'mean':mean,'population_variance':variance,'finite_grid_mean':float((1-F(1,1<<53))/2),'finite_grid_variance':float((1-F(1,1<<106))/12)},
            'gaussian_math_float64_diagnostic':{'mean':nm,'population_variance':nv,'ideal_continuous_mean':0,'ideal_continuous_variance':1,
               'note':'finite open52 tail and ordinary math candidate; not a strict golden sequence'}}
    try:
        tile=json.loads((HERE/'rank_4x4_fixture.json').read_text())['rank']
        volume=json.loads((HERE/'stbn_shape_fixture.json').read_text())['rank']
        report['synthetic_rank_spectrum']=resources.spectral_diagnostics(tile)
        report['synthetic_volume_spectrum']=resources.spectral_diagnostics(volume)
    except ImportError:report['spectrum']='not run: optional numpy absent'
    return report


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test',action='store_true')
    parser.add_argument('--statistics',action='store_true')
    parser.add_argument('--allow-candidate-rng',action='store_true',help='opt into experimental packing; never a production golden')
    parser.add_argument('--report',type=Path)
    parser.add_argument('--input',type=Path,help='oracle request JSONL')
    parser.add_argument('--candidate',type=Path,help='candidate result JSONL, matched by id')
    parser.add_argument('--profile',choices=['strict','accelerated'],default='strict')
    parser.add_argument('--list',action='store_true')
    args=parser.parse_args(argv)
    if args.list:
        print('\n'.join(OPERATIONS));return 0
    report={'schema_version':1,'execution_utc':datetime.now(timezone.utc).isoformat(),
            'python':sys.version,'platform':platform.platform(),'implementation_scope':'independent mathematical oracle only; no Photospider kernel execution',
            'rng_layout_status':'candidate_not_frozen', 'candidate_layout':rng.CANDIDATE_LAYOUT,'production_blue_noise_assets':False}
    report['optional_dependencies']={}
    for package in ('mpmath','numpy'):
        try:report['optional_dependencies'][package]=importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:report['optional_dependencies'][package]=None
    passed=True
    if args.self_test:
        suite=unittest.defaultTestLoader.discover(str(HERE),pattern='test_*.py')
        log=io.StringIO();start=time.perf_counter()
        result=unittest.TextTestRunner(stream=log,verbosity=2).run(suite)
        report['unittest']={'tests_run':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),
                            'skipped':len(result.skipped),'successful':result.wasSuccessful(),'elapsed_seconds':time.perf_counter()-start,
                            'failure_details':[str(x) for x in result.failures+result.errors]}
        report['fixtures']=fixture_check();passed=result.wasSuccessful() and report['fixtures']['passed']
        if args.report:
            args.report.parent.mkdir(parents=True,exist_ok=True)
            args.report.with_suffix('.log').write_text(log.getvalue(),encoding='utf-8')
        print(log.getvalue(),file=sys.stderr,end='')
    if args.statistics:
        if not args.allow_candidate_rng:parser.error('--statistics requires --allow-candidate-rng')
        report['statistics']=statistical_report()
    if args.candidate and not args.input:parser.error('--candidate requires --input')
    if args.input:
        requests=[json.loads(line) for line in args.input.read_text().splitlines() if line.strip()]
        if len({x['id'] for x in requests})!=len(requests):raise ValueError('duplicate request ids')
        results=[evaluate(x,args.allow_candidate_rng) for x in requests]
        if args.candidate:
            candidate=[json.loads(line) for line in args.candidate.read_text().splitlines() if line.strip()]
            if len({x['id'] for x in candidate})!=len(candidate):raise ValueError('duplicate candidate ids')
            indexed={x['id']:x for x in candidate};errors=[]
            if set(indexed)!={x['id'] for x in results}:errors.append('candidate id set mismatch')
            for req,ref in zip(requests,results):
                if ref['id'] in indexed:
                    relaxed=args.profile=='accelerated' and req['op'] in ACCELERATED
                    errors.extend(ref['id']+': '+x for x in compare_tree(ref['result'],indexed[ref['id']].get('result'),relaxed))
            report['candidate_check']={'requests':len(requests),'profile':args.profile,'errors':errors,'passed':not errors,
               'scope':'finite sample comparison; NOT full-domain admission, topology publication certification or kernel integration'}
            passed=passed and not errors
        else:
            for value in results:print(json.dumps(value,ensure_ascii=False))
    if args.report:args.report.parent.mkdir(parents=True,exist_ok=True)
    if args.report:args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    if not (args.self_test or args.input or args.statistics):parser.print_help()
    return 0 if passed else 1


if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,TypeError,OverflowError,ArithmeticError,OSError,json.JSONDecodeError) as exc:
        print(json.dumps({'oracle_error':type(exc).__name__,'detail':str(exc)},ensure_ascii=False),file=sys.stderr)
        raise SystemExit(2)
