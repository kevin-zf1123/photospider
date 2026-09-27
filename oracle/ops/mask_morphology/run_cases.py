#!/usr/bin/env python3
"""Emit/verify mathematical golden vectors or compare normalized candidate output."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import sys
from exact import OracleError, atom, bits
from reference import evaluate

HERE=Path(__file__).resolve().parent
INTEGER_OUTPUT={'label_compact','label_min_pixel','component_count','component_areas','component_bboxes','component_bundle'}
DISTANCE_OUTPUT={'nearest_feature','signed_center_distance','truncated_nearest_feature',
 'truncated_signed_distance','medial_axis_maximal_balls','thin_distance_ordered','offset_polygon_grid','nonzero_to_binary','filter_area_index'}

def encode(case, result):
    dtype=case.get('dtype','float64')
    if case['op'] in DISTANCE_OUTPUT: dtype=case.get('params',{}).get('output_dtype','float64')
    def visit(value, floating, value_dtype=dtype):
        if isinstance(value,dict):
            return {k:visit(v,k in ('values','distance','radius','skeleton','axis','within','valid','fill','barrier','ignored_seeds','reopened_barrier','mean','cholesky'),
                            'float64' if k in ('mean','cholesky') else case.get('dtype','float64') if k in ('skeleton','axis') else value_dtype) for k,v in value.items()}
        if isinstance(value,list): return [visit(v,floating,value_dtype) for v in value]
        if floating and value_dtype!='uint8':
            return 'bits:'+format(bits(atom(value,value_dtype),value_dtype),'08x' if value_dtype=='float32' else '016x')
        if type(value) is bool or not isinstance(value,(int,float)) or int(value)!=value:
            raise OracleError('nonintegral discrete result','TypeMismatch')
        return int(value)
    return visit(result,case['op'] not in INTEGER_OUTPUT)

def outcome(case):
    try: return {'result':encode(case,evaluate(case))}
    except OracleError as e: return {'error':e.code}

def verify_hand(case, result):
    expected=({'error':case['expected_error']} if 'expected_error' in case else
              {'result':encode(case,case['expected'])})
    if result != expected:
        raise AssertionError(f"{case['id']}\nexpected {expected}\nactual   {result}")

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cases',type=Path,default=HERE/'cases.json')
    parser.add_argument('--golden',type=Path,default=HERE/'golden.json')
    parser.add_argument('--emit',action='store_true',help='Write new golden; never default.')
    parser.add_argument('--output',type=Path,help='Required destination for --emit.')
    parser.add_argument('--candidate',type=Path,help='JSON map case-ID -> normalized result/error envelope.')
    parser.add_argument('--case',type=Path,help='Evaluate one arbitrary case, not a WorkflowDocument.')
    parser.add_argument('--verify',action='store_true',help='Verify checked-in cases and golden (default).')
    args=parser.parse_args()
    if args.case:
        print(json.dumps(outcome(json.loads(args.case.read_text())),indent=2)); return 0
    cases=json.loads(args.cases.read_text())['cases']
    actual={}
    for case in cases:
        if case['id'] in actual: raise AssertionError('duplicate case ID')
        actual[case['id']]=outcome(case)
        verify_hand(case,actual[case['id']])
    if args.emit:
        if not args.output: parser.error('--emit requires explicit --output')
        args.output.write_text(json.dumps(dict(schema_version=1,results=actual),indent=2)+'\n')
        print(f'Wrote {len(actual)} hand-checked vectors: {args.output}'); return 0
    stored=json.loads(args.golden.read_text())['results']
    if actual != stored: raise AssertionError('checked-in golden mismatch')
    if args.candidate:
        candidate=json.loads(args.candidate.read_text())['results']
        canonical = lambda v: json.dumps(v,sort_keys=True,separators=(',',':'),allow_nan=False)
        if canonical(candidate) != canonical(stored):
            raise AssertionError('candidate strict mismatch (keys, types or exact bits)')
    print(f'PASS: {len(cases)} analytic/domain vectors; {len({c["op"] for c in cases})} members; strict golden unchanged.')
    return 0

if __name__=='__main__':
    try: sys.exit(main())
    except (OracleError,AssertionError,ValueError,KeyError,TypeError,OSError) as error:
        print(str(error),file=sys.stderr); sys.exit(1)
