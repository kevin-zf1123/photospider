"""Pinned third-party comparison manifests. No algorithm is executed.

A valid manifest means only 'identity fields supplied', not licensed, executed,
NUM-conformant, or golden-approved. Actual resource bytes must be independently
provided and hash-checked by the integrating application.
"""
from __future__ import annotations
import re
from .core import DomainError

COMMON=('implementation_name','implementation_version','source_or_binary_sha256','license_review_reference',
        'algorithm_profile','input_units','parameter_snapshot','platform_snapshot',
        'precision_profile','comparison_dataset_sha256')
SMAA=('shader_sha256','area_texture_sha256','search_texture_sha256','sampling_state',
      'viewport_mapping','shader_macros','edge_detection_profile')
BM3D=('block_shape','block_stride','search_shape','max_group_size','distance_rule',
      'matching_tie_rule','transforms_and_normalization','hard_threshold_rule',
      'wiener_pilot_rule','aggregation_weights','boundary_padding')

def validate_manifest(manifest,engine):
    if engine not in ('smaa_1x','bm3d','cbm3d'):raise DomainError('comparison family')
    required=COMMON+(SMAA if engine=='smaa_1x' else BM3D)+(('color_transform',) if engine=='cbm3d' else ())
    missing=[k for k in required if k not in manifest or manifest[k] in (None,'',{})]
    malformed=[k for k in required if k.endswith('sha256') and k in manifest and not re.fullmatch('[0-9a-f]{64}',str(manifest[k]))]
    return {'engine':engine,'manifest_complete':not(missing or malformed),'missing':missing,'malformed_digests':malformed,
            'resource_bytes_verified':False,'comparison_executed':False,'license_approved':False,
            'numeric_conformance_proven':False,'evidence_status':'pending_actual_comparison'}

def smaa(manifest):return validate_manifest(manifest,'smaa_1x')
def bm3d(manifest):return validate_manifest(manifest,'bm3d')
def cbm3d(manifest):return validate_manifest(manifest,'cbm3d')

def golden_template(engine):
    required=COMMON+(SMAA if engine=='smaa_1x' else BM3D)+(('color_transform',) if engine=='cbm3d' else ())
    return {'schema':'FilterComparison/v1','engine':engine,'status':'pending','manifest':{k:None for k in required},
            'cases':[{'case_id':name,'input_tensor_file':None,'input_sha256':None,'parameters':None,
                      'expected_tensor_file':None,'expected_sha256':None,'comparison_rule':None,'run_log':None}
                     for name in ('identity_or_disabled','constant','impulse','diagonal_edge','fine_texture','boundary_crop','seeded_noise')],
            'note':'No expected samples were generated. Do not replace these nulls with another algorithm output.'}
