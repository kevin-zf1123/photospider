"""Hand-derived fixtures: not outputs copied from the implementation under test."""
from __future__ import annotations
import copy


def examples():
    cases = []
    def add(op, inputs, params, expected, note, dtype='float64', suffix='analytic'):
        cases.append(dict(id=op+'.'+suffix, op=op, dtype=dtype,
                          inputs=inputs, params=params, expected=expected, evidence=note))
    a = [[0, 1, 0, 1]]; b = [[0, 0, 1, 1]]
    add('binary_logic', dict(a=a,b=b), dict(operation='xor'), [[0,1,1,0]], 'Boolean XOR truth table.')
    add('fuzzy_logic', dict(a=[[.25,.75]],b=[[.5,.25]]), dict(operation='subtract'), [[.25,.75]], 'min(a,1-b).')
    add('independent_coverage', dict(a=[[.5]],b=[[.5]]), dict(operation='or'), [[.75]], '1/2+1/2-1/4=3/4.')
    add('invert', dict(input=[[0,.25,1]]), {}, [[1,.75,0]], 'Exact complements.')
    add('threshold', dict(input=[[-1,.5,2]]), dict(threshold=.5,comparison='ge'), [[0,1,1]], 'Inclusive threshold equality.')
    add('range_mask',dict(input=[[0,.5,1]]),dict(lower=0,upper=1,lower_closed=False,upper_closed=False),[[0,1,0]],'Both endpoints open.')
    add('soft_threshold',dict(input=[[-1,0,1]]),dict(threshold=0,width=2,curve='smoothstep'),[[0,.5,1]],'Cubic ramp endpoints and midpoint.')
    add('soft_range',dict(input=[[-1,0,1,2,3]]),dict(lower=0,upper=2,lower_width=2,upper_width=2,curve='linear'),[[0,.5,1,.5,0]],'Min of rising and falling linear ramps.')
    add('nonzero_to_binary',dict(input=[[0,1,2,255]]),dict(output_dtype='float64'),[[0,1,1,1]],'Explicit UInt8 truthiness adapter.',dtype='uint8')
    add('coordinate_range',dict(image=[[[0,1,2]]],target=[0],scales=[1]),dict(channels='0',inner=0,outer=2,curve='linear'),[[1,.5,0]],'One-coordinate distances 0,1,2.')
    add('lab76_range',dict(image=[[[0,.5,1]],[[0,0,0]],[[0,0,0]]],target=[0,0,0]),dict(channels='0,1,2',inner=0,outer=100,curve='linear'),[[1,.5,0]],'Stored l is multiplied by100 in delta-L*.')
    add('hue_range',dict(image=[[[.875,0,.5]],[[1,1,1]]]),dict(channels='0,1',center_turns=0,inner=0,outer=.25,curve='linear',minimum_chroma=0,neutral_policy='exclude'),[[.5,1,0]],'Wrapped distance of7/8 to0 is1/8.')
    add('dilate',dict(input=[[0,1,0]]),dict(radius=1),[[1,1,1]],'Radius1 square reaches one cell left/right.')
    add('erode',dict(input=[[1]]),dict(radius=1),[[0]],'Zero exterior wins min.')
    add('opening',dict(input=[[1]]),dict(radius=1),[[0]],'Point cannot contain a radius1 square.')
    add('closing',dict(input=[[1]]),dict(radius=1),[[1]],'Infinite-lattice dilation extends beyond canvas before erosion.')
    add('offset_discrete',dict(input=[[0,1,0]]),dict(radius=1,metric='l2',sy=1,sx=1),[[1,1,1]],'One-unit positive discrete offset.')
    add('shift_distance_field',dict(input=[[-1,0,1]]),dict(radius=.5),[[-1.5,-.5,.5]],'Level-field d-r, not recomputed SDF.')
    add('threshold_distance_field',dict(input=[[-1,0,1]]),dict(radius=0),[[1,1,0]],'Closed sublevel set d<=0.')
    add('offset_polygon_grid',dict(vertices=[[-.5,-.5],[-.5,.5],[.5,.5],[.5,-.5]]),dict(H=1,W=1,radius=0,sy=1,sx=1,samples_per_axis=2,output_dtype='float64'),[[1]],'All four fixed subpixel centres lie in the square.')
    add('gaussian_feather',dict(input=[[1,1],[1,1]]),dict(sigma_y=1,sigma_x=1,radius_y=1,radius_x=1,boundary='replicate'),[[1,1],[1,1]],'Every mapped tap has value1; numerator equals denominator exactly.')
    for name in ('distance_feather_linear','distance_feather_smoothstep'):
        add(name,dict(input=[[-1,0,1]]),dict(inner=1,outer=1),[[1,.5,0]],'Negative-inside distance, endpoints and midpoint.')
    add('nearest_feature',dict(input=[[1,0,1]]),dict(feature='foreground',metric='l2',sy=1,sx=1,exterior='none',squared=False,output_dtype='float64'),dict(distance=[[0,1,0]],nearest=[[[0,0,0]],[[0,0,2]]]),'Equal centre distances choose lexicographically smaller(y,x).')
    add('signed_center_distance',dict(input=[[0,1,0]]),dict(metric='l2',sy=1,sx=1,exterior='background',output_dtype='float64'),dict(distance=[[1,-1,1]]),'Each pixel is one unit from opposite class; inside negative.')
    add('truncated_nearest_feature',dict(input=[[1,0,0]]),dict(limit=1,feature='foreground',metric='l2',sy=1,sx=1,exterior='none',squared=False,output_dtype='float64'),dict(distance=[[0,1,1]],within=[[1,1,0]]),'Limit equality is within; farther distances clamp.')
    add('truncated_signed_distance',dict(input=[[0,1,0]]),dict(limit=.5,metric='l2',sy=1,sx=1,exterior='background',output_dtype='float64'),dict(distance=[[.5,-.5,.5]],within=[[0,0,0]]),'No opposite centre lies within half-unit.')
    img=[[[0,1,2,3]]]; seeds=[[1,0,0,0]]; barrier=[[0,0,0,0]]
    add('flood_fixed',dict(image=img,seeds=seeds,barrier=barrier),dict(tolerance=1,connectivity=4),[[1,1,0,0]],'Fixed seed reference0 rejects2.')
    add('flood_neighbor',dict(image=img,seeds=seeds,barrier=barrier),dict(tolerance=1,connectivity=4),[[1,1,1,1]],'Each adjacent difference1 permits propagation.')
    add('flood_barrier',dict(seeds=seeds,barrier=[[0,0,1,0]]),dict(connectivity=4),[[1,1,0,0]],'Barrier blocks the one-dimensional path.')
    binary=[[1,0,1],[1,0,0]]; labels=[[1,0,3],[1,0,0]]
    add('label_compact',dict(input=binary),dict(connectivity=4,maximum_count=6),[[1,0,2],[1,0,0]],'First row-major sites define compact IDs.')
    add('label_min_pixel',dict(input=binary),dict(connectivity=4,maximum_count=6),labels,'Minimum linear indices0,2 give IDs1,3.')
    add('component_count',dict(labels=labels),{},[2],'Distinct positive IDs count; max-ID3 is not count.')
    add('component_areas',dict(labels=labels),{},dict(rows=[[1,2],[3,1]]),'Exact pixel counts by sorted ID.')
    add('component_bboxes',dict(labels=labels),{},dict(rows=[[1,0,0,2,1],[3,0,2,1,3]]),'Half-open bbox coordinates.')
    add('component_bundle',dict(input=binary),dict(connectivity=4,maximum_count=6),dict(labels=[1,0,3,1,0,0],rows=[[1,2,0,0,0,2,1],[3,1,2,0,2,1,3]]),'Combined MinPixel label and attribute schema.')
    add('filter_area_index',dict(labels=labels,area_index=dict(source_id='object-A',rows=[[1,2],[3,1]])),dict(labels_id='object-A',minimum_area=2),[[1,0,0],[1,0,0]],'Area equal to minimum is retained, foreign IDs are not implied.')
    ring=[[1,1,1],[1,0,1],[1,1,1]]
    add('fill_holes',dict(input=ring),dict(foreground_connectivity=4),[[1,1,1],[1,1,1],[1,1,1]],'One enclosed background pixel.')
    add('remove_small',dict(input=binary),dict(maximum_removed_area=2,foreground_connectivity=4),[[0,0,0],[0,0,0]],'Remove area<=2, including equality.')
    add('fill_small_holes',dict(input=ring),dict(maximum_area=1,foreground_connectivity=4),[[1,1,1],[1,1,1],[1,1,1]],'Fill hole area<=1 including equality.')
    for name,expected in [('morph_gradient',1),('inner_border',1),('outer_border',0),('white_top_hat',1),('black_top_hat',0)]:
        add(name,dict(input=[[1]]),dict(radius=1),[[expected]],'Point dilation1, erosion0, opening0, closing1.')
    add('thin_topological',dict(input=[[1,1,1]]),{},[[1,1,1]],'Endpoints preserved and deleting centre would split component.')
    add('thin_distance_ordered',dict(input=[[1,1,1]]),dict(sy=1,sx=1,output_dtype='float64'),dict(skeleton=[[1,1,1]],radius=[[1,1,1]]),'Line retained; exterior row at distance1 supplies radius.')
    add('reconstruct_dilate',dict(marker=[[1,0,0]],limit=[[1,.5,.5]]),dict(connectivity=4),[[1,.5,.5]],'Propagation capped by limit at half.')
    add('reconstruct_erode',dict(marker=[[0,1,1]],limit=[[0,.5,.5]]),dict(connectivity=4),[[0,.5,.5]],'Dual propagation floored by limit at half.')
    add('bridge_axis_gaps',dict(barrier=[[1,0,1,0,0,1]]),dict(maximum_gap=1),[[1,1,1,0,0,1]],'Only bounded zero-run length1 is filled, length2 remains.')
    add('fill_axis_gaps',dict(barrier=[[1,0,1,0,0,1]],seeds=[[0,0,0,1,0,0]]),dict(maximum_gap=1,fill_connectivity=4),[[0,0,0,1,1,0]],'Flood the surviving length2 run on temporary barrier.')
    add('fill_morphological_gaps',dict(barrier=[[1,0,0,1]],seeds=[[0,1,0,0]]),dict(radius=0,fill_connectivity=4),[[0,1,1,0]],'Radius0 leaves original barrier and floods enclosed run.')
    add('affect_result',dict(original=[[[2,2,2]]],processed=[[[10,10,10]]],mask=[[0,.5,1]]),dict(channels='0'),[[[2,6,10]]],'Complete convex expression at endpoints and midpoint.')
    add('multiply_mask',dict(image=[[[2,2,2]]],mask=[[0,.5,1]]),dict(channels='0'),[[[0,1,2]]],'Complete product at zero, half and one weights.')
    add('restricted_mean',dict(image=[[[2,4,8]]],mask=[[1,0,1]]),dict(radius=1,empty_policy='zero',request='both'),dict(values=[[[2,5,8]]],valid=[[1,1,1]]),'Zero-weight finite4 contributes zero; centre mean is(2+8)/2.')
    add('apply_alpha',dict(image=[[[5,7]],[[.5,1]]],mask=[[.5,0]]),dict(alpha_channel=1),[[[5,7]],[[.25,0]]],'Straight color unchanged; alpha multiplied.')
    for name in ('thin_zhang_suen','thin_guo_hall'):
        add(name,dict(input=[[1,1,1]]),{},[[1,1,1]],'Horizontal chain is a fixed point.')
        add(name,dict(input=[[1]]),{},[[1]],'Single pixel is retained.',suffix='single')
    add('thin_zhang_suen',dict(input=[[1,1],[1,1]]),{},[[0,0],[0,0]],
        'All four pixels satisfy the first synchronous deletion phase.',suffix='block2')
    add('thin_guo_hall',dict(input=[[1,1],[1,1]]),{},[[0,1],[0,0]],
        'Phase0 removes left column; phase1 removes bottom-right.',suffix='block2')
    for metric in ('l1','l2','linf'):
        add('medial_axis_maximal_balls',dict(input=[[1]]),dict(metric=metric,sy=2,sx=3,output_dtype='float64'),
            dict(axis=[[1]],radius=[[2]]),'Nearest exterior center is two units away.',suffix='single.'+metric)
        add('medial_axis_maximal_balls',dict(input=[[0,0]]),dict(metric=metric,output_dtype='float64'),
            dict(axis=[[0,0]],radius=[[0,0]]),'Empty foreground has no centers.',suffix='empty.'+metric)
        add('medial_axis_maximal_balls',dict(input=[[1,1],[1,1]]),dict(metric=metric,output_dtype='float64'),
            dict(axis=[[1,1],[1,1]],radius=[[1,1],[1,1]]),'Four singleton balls are mutually non-containing.',suffix='block2.'+metric)
    add('medial_axis_maximal_balls',dict(input=[[1,1,1]]*3),dict(metric='linf',output_dtype='float64'),
        dict(axis=[[0,0,0],[0,1,0],[0,0,0]],radius=[[0,0,0],[0,2,0],[0,0,0]]),
        'Central open square contains every other candidate ball.',suffix='full3_linf')
    add('lab2000_range',dict(image=[[[.25,.75]],[[0,0]],[[0,0]]],target=[.25,0,0]),
        dict(channels='0,1,2',kL=1,kC=1,kH=1,inner=0,outer=100,curve='linear'),[[1,.5]],
        'Neutral symmetric L*=25/75 gives exact distance50.',suffix='neutral')
    add('lab2000_range',dict(image=[[[.75]],[[0]],[[0]]],target=[.25,0,0]),
        dict(kL=2,kC=1,kH=1,inner=0,outer=100,curve='linear'),[[.75]],
        'Pure lightness distance50 divided by kL2.',suffix='nonunit')
    model=dict(ids=[7],mean=[[1,2,3]],cholesky=[[[1,0,0],[0,2,0],[0,0,3]]])
    add('fit_color_groups_table',dict(samples=[[1,2,3],[4,5,6]],group_ids=[7,11],weights=[2,0],epsilon=[1,2,3]),{},
        dict(model=model,skipped_group_ids=[11]),'Single contributing color; diagonal factor equals epsilon.')
    add('fit_color_groups_image',dict(image=[[[1,4]],[[2,5]],[[3,6]]],group_ids=[[7,11]],weights=[[2,0]],epsilon=[1,2,3]),
        dict(channels='0,1,2'),dict(model=model,skipped_group_ids=[11]),'Image and table admit identical samples.')
    add('apply_color_groups',dict(image=[[[1,2,3]],[[2,2,2]],[[3,3,3]]],model=model),
        dict(channels='0,1,2',inner=0,outer=2,curve='linear'),[[1,.5,0]],'Triangular solve gives distances0,1,2.')
    add('hue_range',dict(image=[[[.5,0]],[[0,.25]]]),dict(center_turns=0,minimum_chroma=0,neutral_policy='include',inner=0,outer=.1),
        [[1,1]],'Exact zero chroma uses include even when threshold is zero.',suffix='zero_neutral')
    # Every floating member repeats its analytic case at Float32 storage.
    originals=list(cases)
    for case in originals:
        if case['dtype'] != 'float64' or case['op'].startswith('component_') or case['op']=='filter_area_index': continue
        c=copy.deepcopy(case); c['dtype']='float32'; c['id']+='.f32'
        if 'output_dtype' in c['params']: c['params']['output_dtype']='float32'
        cases.append(c)
    # Edge fixtures explicitly retain difficult classification/copy behavior.
    add('nearest_feature',dict(input=[[0,0]]),dict(feature='foreground',output_dtype='float64'),dict(distance=[['bits:7ff0000000000000','bits:7ff0000000000000']],nearest=[[[-1,-1]],[[-1,-1]]]),'No feature gives positive infinity.',suffix='no_feature')
    add('signed_center_distance',dict(input=[[1]]),dict(exterior='none',output_dtype='float64'),dict(distance=[['bits:fff0000000000000']]),'Foreground without background gives negative infinity.',suffix='no_opposite')
    add('component_areas',dict(labels=[[0]]),{},dict(rows=[]),'RuntimeCount zero; no background row.',suffix='empty_result')
    add('component_count',dict(labels=[[8,0,8]]),{},[1],'Repeated imported label is one declared object.',suffix='disconnected_id')
    add('gaussian_feather',dict(input=[['bits:8000000000000000']]),dict(sigma_y=0,sigma_x=0,radius_y=0,radius_x=0),[['bits:8000000000000000']],'Two zero radii perform bit-copy including negative zero.',suffix='identity_negative_zero')
    add('affect_result',dict(original=[[[2,'bits:7ff8000000000001']]],processed=[[['bits:7ff8000000000002',7]]],mask=[[0,1]]),dict(channels='0'),[[[2,7]]],'Unselected branch NaNs do not participate.',suffix='unselected_nan')
    add('multiply_mask',dict(image=[[['bits:7ff8000000000001',-0.0]]],mask=[[0,1]]),dict(channels='0'),[[[0,'bits:8000000000000000']]],'Zero gate suppresses NaN; one copies signed zero.',suffix='gating')
    add('restricted_mean',dict(image=[[['bits:7ff8000000000001',6]]],mask=[[0,1]]),dict(radius=1,empty_policy='zero'),dict(values=[[[6,6]]],valid=[[1,1]]),'Zero-weight NaN is excluded.',suffix='masked_nan')
    add('restricted_mean',dict(image=[[['bits:7ff8000000000001']]],mask=[[1]]),dict(radius=0,request='valid',empty_policy='error'),dict(valid=[[1]]),'Valid-only requires weights, not finite image samples.',suffix='valid_only')
    add('fill_holes',dict(input=[[0,1,1],[1,0,1],[1,1,1]]),dict(foreground_connectivity=4),[[0,1,1],[1,0,1],[1,1,1]],'Background8 connects centre diagonally to canvas.',suffix='complementary8')
    add('fill_holes',dict(input=[[0,1,1],[1,0,1],[1,1,1]]),dict(foreground_connectivity=8),[[0,1,1],[1,1,1],[1,1,1]],'Background4 makes centre an enclosed hole.',suffix='complementary4')
    add('dilate',dict(input=[['bits:8000000000000000',0]]),dict(radius=1),[['bits:8000000000000000',0]],'Equal extrema choose centre first.',suffix='signed_zero_tie')
    add('offset_polygon_grid',dict(vertices=[[-.5,-.5],[-.5,0],[.5,0],[.5,-.5]]),dict(H=1,W=1,samples_per_axis=2),[[.5]],'Two of four fixed samples lie in the half-square.',suffix='half_coverage')
    add('threshold',dict(input=[[1]]),dict(threshold='bits:3ff0000000000001'),[[0]],'Float64 static threshold is not narrowed to input Float32.',dtype='float32',suffix='f64_static')
    for dtype in ('float32','float64'):
        add('remove_small',dict(input=[[1,0,1,1,0,1,1,1]]),dict(maximum_removed_area=2),
            [[0,0,0,0,0,1,1,1]],'Areas 1 and 2 removed; area 3 retained.',dtype=dtype,suffix='inclusive.'+dtype)
        add('remove_small',dict(input=[[1,0,1]]),dict(maximum_removed_area=0),[[1,0,1]],
            'Zero maximum removes no nonempty component.',dtype=dtype,suffix='zero.'+dtype)
    cases.append(dict(id='remove_small.negative_maximum',op='remove_small',dtype='float64',
                      inputs=dict(input=[[1]]),params=dict(maximum_removed_area=-1),
                      expected_error='InvalidDomain',evidence='Negative area limit rejected.'))
    # Invalid-domain cases expect project oracle categories, not a public ABI enum.
    errors=[
      ('binary_logic',dict(a=[[2]],b=[[0]]),{},'InvalidDomain'),
      ('invert',dict(input=[[1.01]]),{},'InvalidDomain'),
      ('soft_threshold',dict(input=[[0]]),dict(width=-1),'InvalidDomain'),
      ('range_mask',dict(input=[[0]]),dict(lower=2,upper=1),'InvalidDomain'),
      ('dilate',dict(input=[[0]]),dict(footprint='custom',offsets='0:0;0:1'),'InvalidDomain'),
      ('dilate',dict(input=[[0]]),dict(footprint='custom',offsets='-0:0'),'InvalidDomain'),
      ('nearest_feature',dict(input=[[0]]),dict(sy=0),'InvalidDomain'),
      ('truncated_nearest_feature',dict(input=[[0]]),dict(limit=-1),'InvalidDomain'),
      ('gaussian_feather',dict(input=[[0]]),dict(sigma_y=0,radius_y=1),'InvalidDomain'),
      ('flood_barrier',dict(seeds=[[1]],barrier=[[1]]),{},'InvalidDomain'),
      ('label_compact',dict(input=[[1]]),dict(maximum_count=0),'InvalidDomain'),
      ('filter_area_index',dict(labels=[[1]],area_index=dict(source_id='B',rows=[[1,1]])),dict(labels_id='A',minimum_area=1),'InvalidAssociation'),
      ('filter_area_index',dict(labels=[[1]],area_index=dict(source_id='A',rows=[[1,1.0]])),dict(labels_id='A',minimum_area=1),'InvalidDomain'),
      ('component_count',dict(labels=[[-1]]),{},'InvalidDomain'),
      ('reconstruct_dilate',dict(marker=[[1]],limit=[[0]]),{},'InvalidDomain'),
      ('restricted_mean',dict(image=[[[2]]],mask=[[0]]),dict(radius=0,empty_policy='error'),'InvalidDomain'),
      ('affect_result',dict(original=[[['bits:7ff8000000000001']]],processed=[[[1]]],mask=[[.5]]),dict(channels='0'),'InvalidDomain'),
      ('offset_polygon_grid',dict(vertices=[[0,0],[1,1],[0,1],[1,0]]),dict(H=1,W=1),'InvalidDomain'),
    ]
    for i,(op,inputs,params,error) in enumerate(errors):
        cases.append(dict(id=op+f'.invalid.{i:02}',op=op,dtype='float64',inputs=inputs,params=params,
                          expected_error=error,evidence='Explicit forbidden domain/schema/association.'))
    for case in cases:
        if case['id'] in ('affect_result.unselected_nan','multiply_mask.gating','restricted_mean.masked_nan'):
            case.pop('expected')
            case['expected_error']='InvalidDomain'
            case['evidence']='Zero coefficients still consume and reject nonfinite arithmetic operands.'
    for dtype in ('float32','float64'):
        add('multiply_mask',dict(image=[[[-0.0,-0.0]]],mask=[[0,1]]),dict(channels='0'),[[[0,0]]],
            'Arithmetic zero is +0; no endpoint bit-copy.',dtype=dtype,suffix='arithmetic_zero.'+dtype)
        for op,inputs,params in [
            ('affect_result',dict(original=[[[float('inf')]]],processed=[[[2]]],mask=[[1]]),dict(channels='0')),
            ('multiply_mask',dict(image=[[[float('inf')]]],mask=[[0]]),dict(channels='0')),
            ('restricted_mean',dict(image=[[[float('inf')]]],mask=[[0]]),dict(radius=0,empty_policy='zero'))]:
            # Portable JSON uses explicit IEEE bits for nonfinite samples.
            def portable(x):
                if isinstance(x,dict): return {k:portable(v) for k,v in x.items()}
                if isinstance(x,list): return [portable(v) for v in x]
                return ('bits:7f800000' if dtype=='float32' else 'bits:7ff0000000000000') if x==float('inf') else x
            cases.append(dict(id=op+'.zero_weight_inf.'+dtype,op=op,dtype=dtype,inputs=portable(inputs),
                              params=params,expected_error='InvalidDomain',evidence='All arithmetic operands are validated.'))
    for case in cases:
        if case['op']=='offset_polygon_grid':
            case['params'].setdefault('sample_pattern','grid_center')
            case['params'].setdefault('samples_per_axis',4)
    for dtype in ('float32','float64'):
        for n,hits in ((1,0),(2,0),(4,1),(8,2),(16,4)):
            # Left quarter excluding x=1/4: bound u=3/16, inclusive.
            add('offset_polygon_grid',dict(vertices=[[-.5,-.5],[-.5,-.3125],[.5,-.3125],[.5,-.5]]),
                dict(H=1,W=1,sample_pattern='vulkan_standard',sample_count=n,output_dtype=dtype),
                [[hits/n]],'Exact standard-table horizontal counts at u<=3/16.',dtype=dtype,suffix='standard'+str(n)+'.'+dtype)
    for dtype in ('float32','float64'):
        for op,params in [('fill_axis_gaps',dict(maximum_gap=2)),('fill_morphological_gaps',dict(radius=1))]:
            add(op,dict(barrier=[[1,0,0,1]],seeds=[[0,1,0,0]]),params,[[0,1,0,0]],
                'Only the seeded newly closed pixel reopens; neighboring closure stays.',dtype=dtype,suffix='seed_override.'+dtype)
            add(op,dict(barrier=[[1,0,0,1]],seeds=[[0,1,1,0]]),params,[[0,1,1,0]],
                'All seed-site additions cleared simultaneously.',dtype=dtype,suffix='multi_seed_override.'+dtype)
            cases.append(dict(id=op+'.original_barrier_seed.'+dtype,op=op,dtype=dtype,
                              inputs=dict(barrier=[[1,0,1]],seeds=[[1,0,0]]),params=params,
                              expected=[[0,0,0]],evidence='All seeds on original barrier are ignored; barrier retained.'))
    for dtype in ('float32','float64'):
        for op,params in [('fill_axis_gaps',dict(maximum_gap=2)),('fill_morphological_gaps',dict(radius=1))]:
            add(op,dict(barrier=[[1,0,0,1]],seeds=[[1,1,0,0]]),params,[[0,1,0,0]],
                'Original-barrier seed ignored, surviving seed reopens only its added barrier.',dtype=dtype,suffix='mixed_seed_filter.'+dtype)
    # Hand-derived diagnostics for the fixed one-row gap examples above.
    for case in cases:
        if case['op'] not in ('fill_axis_gaps','fill_morphological_gaps') or 'expected' not in case:continue
        cid=case['id']; fill=case['expected']
        if '.analytic' in cid:
            barrier=[[1,1,1,0,0,1]] if case['op']=='fill_axis_gaps' else [[1,0,0,1]]
            ignored=[[0]*len(barrier[0])];reopened=[[0]*len(barrier[0])]
        elif '.original_barrier_seed' in cid:
            barrier=[[1,1,1]];ignored=[[1,0,0]];reopened=[[0,0,0]]
        elif '.multi_seed_override' in cid:
            barrier=[[1,0,0,1]];ignored=[[0,0,0,0]];reopened=[[0,1,1,0]]
        else:
            barrier=[[1,0,1,1]];reopened=[[0,1,0,0]]
            ignored=[[1,0,0,0]] if '.mixed_seed_filter' in cid else [[0,0,0,0]]
        case['expected']=dict(fill=fill,barrier=barrier,ignored_seeds=ignored,reopened_barrier=reopened)
    for op,params in [('fill_axis_gaps',dict(maximum_gap=2)),('fill_morphological_gaps',dict(radius=1))]:
        add(op,dict(barrier=[[1,0,0,1]],seeds=[[0,0,0,0]]),params,
            dict(fill=[[0,0,0,0]],barrier=[[1,1,1,1]],ignored_seeds=[[0,0,0,0]],reopened_barrier=[[0,0,0,0]]),
            'No seeds: closure diagnostics still returned.',suffix='empty_seed_diagnostics')
    return cases
