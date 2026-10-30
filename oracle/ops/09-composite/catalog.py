"""Reusable mathematical entry points and authored small reference samples."""
from functools import partial
from reference import blend as b, keying as k
from reference.exact import F, q, rn

REFERENCES={}
SAMPLES={}
EXPECTED={}


def add(ident,fn,args=(),kwargs=None,expected=None):
    REFERENCES[ident]=fn
    SAMPLES[ident]=(args,kwargs or {})
    if expected is not None:EXPECTED[ident]=expected


def clear(template,dtype='f64'):
    return [0.]*len(template)


def copy(value,dtype='f64'):
    return [q(v,dtype) for v in value]


def matting(image,trimap,epsilon,dtype='f64'):
    alpha,diagnostics=k.closed_form_matting(image,trimap,epsilon,dtype)
    residual=max((abs(r) for r in diagnostics['exact_raw_residual']),default=F(0))
    return {'matte':alpha,'raw_residual':rn(residual,'f64')}


source=[1,0,0,.5]
background=[0,0,1,.5]
add('CMP-01A',clear,(source,),expected=[0]*4)
add('CMP-02A',copy,(source,),expected=source)
add('CMP-02B',copy,(background,),expected=background)
pd=[('03A','03D','source_over',[F(2,3),0,F(1,3),F(3,4)]),
    ('03B','03E','destination_over',[F(1,3),0,F(2,3),F(3,4)]),
    ('04A','04C','source_in',[1,0,0,F(1,4)]),
    ('04B','04D','destination_in',[0,0,1,F(1,4)]),
    ('05A','05C','source_out',[1,0,0,F(1,4)]),
    ('05B','05D','destination_out',[0,0,1,F(1,4)]),
    ('06A','06C','source_atop',[F(1,2),0,F(1,2),F(1,2)]),
    ('06B','06D','destination_atop',[F(1,2),0,F(1,2),F(1,2)]),
    ('07A','07B','xor',[F(1,2),0,F(1,2),F(1,2)]),
    ('08A','08C','plus_raw',{'color_mass':[F(1,2),0,F(1,2)],'mass':1}),
    ('08B','08D','plus_lighter_bounded',[F(1,2),0,F(1,2),1])]
for linear,encoded,mode,expected in pd:
    for ident in [linear,encoded]:add('CMP-'+ident,partial(b.composite,mode=mode),(source,background),expected=expected)
for ident in ['CMP-03C','CMP-03F']:
    add(ident,b.composite_blended,(source,background,[1,0,0]),expected=[F(2,3),0,F(1,3),F(3,4)])

separable=[('01A','normal',F(3,4)),('02A','add',1),('02B','subtract',F(-1,2)),('02C','divide',F(1,3)),
 ('03A','multiply',F(3,16)),('03B','multiply_unbounded',F(3,16)),('04A','darken',F(1,4)),('04B','lighten',F(3,4)),
 ('05A','screen',F(13,16)),('05B','screen_unbounded',F(13,16)),('06A','overlay',F(3,8)),('07A','hard_light',F(5,8)),
 ('08A','soft_light_w3c',F(3,8)),('09A','color_dodge',1),('10A','color_burn',0),('11A','linear_dodge',1),('11B','linear_burn',0),
 ('12A','linear_light',F(3,4)),('13A','vivid_light',F(1,2)),('14A','pin_light',F(1,2)),('15A','difference',F(1,2)),
 ('15B','exclusion',F(5,8)),('16A','hard_mix_vivid',1),('16B','hard_mix_sum',1)]
for ident,mode,expected in separable:
    add('BLN-'+ident,partial(b.blend_channel,mode=mode),(.25,.75),expected=expected)
cb=[.5,.25,.75];cs=[.25,.75,.5]
for suffix,measure,darker in [('A','linear_y',True),('B','linear_y',False),('C','oklab_l',True),('D','oklab_l',False),('E','cielab_l',True),('F','cielab_l',False)]:
    mode=('darker' if darker else 'lighter')+'_color_'+measure
    # Equal-weight RGB makes these samples tie; native L still selects on coordinate 0.
    args={'y_weights':[F(1,4),F(1,2),F(1,4)]} if measure=='linear_y' else {}
    expected=(cb if darker else cs) if measure=='linear_y' else (cs if darker else cb)
    add('BLN-17'+suffix,partial(b.blend_color,mode=mode,**args),(cb,cs),expected=expected)
add('BLN-18A',b.dissolve,([1,0,0,1],background),expected=[1,0,0,1])
add('BLN-19A',b.emission_add,(background,[.5,0,0],1),expected=[.5,0,1,.5])
for ident in ['BLN-20A','BLN-20B']:
    add(ident,b.premul_mix,(source,background,.5),expected=[.5,0,.5,.5])

native_b=[.5,3,4];native_s=[.25,0,2]
for family,mode in [('01','hue'),('02','chroma'),('03','color'),('04','lightness')]:
    for suffix,model in [('A','lab'),('B','oklab'),('C','w3c')]:
        if model=='w3c':
            values=([.5,.5,.5],[.75,.25,.5])
            expected={'hue':[F(1,2)]*3,'chroma':[F(1,2)]*3,'color':[F(329,400),F(129,400),F(229,400)],'lightness':[F(171,400)]*3}[mode]
        else:
            values=(native_b,native_s)
            expected={'hue':[F(1,2),0,5],'chroma':[F(1,2),F(6,5),F(8,5)],'color':[F(1,2),0,2],'lightness':[F(1,4),3,4]}[mode]
        add('NBL-'+family+suffix,partial(b.nonseparable,mode=mode,model=model),values,expected=expected)
for suffix,model,policy in [('D','lab','preserve_gray'),('E','oklab','preserve_gray'),('F','lab','source_hue'),('G','oklab','source_hue')]:
    expected=[.5,0,0] if policy=='preserve_gray' else [.5,0,1]
    add('NBL-02'+suffix,partial(b.nonseparable,mode='chroma',model=model,neutral_policy=policy),([.5,0,0],[.25,0,1]),expected=expected)

add('KEY-01A',k.range_select,(.5,.25,.75,0),expected=1)
add('KEY-02A',k.lab76_key,([.5,0,0],[.25,0,0],0,10),expected=1)
add('KEY-02B',k.lab_chroma_key,([.5,0,0],[.25,0,0],0,10),expected=0)
add('KEY-03A',k.difference_key,([.5,0,0],[0,0,0],0,1),expected=.5)
add('KEY-04A',k.clean_plate_nearest,([[[1,0,0],[0,0,1]]],[[1,0]]),expected=([[[1,0,0],[1,0,0]]],[[1,0]]))
add('KEY-04B',k.temporal_median,([[[[.25]*3]],[[[.75]*3]]],[[[1]],[[1]]]),expected=([[[.5]*3]],[[1]]))
add('KEY-05A',matting,([[[.5]*3]*3],[[0,1,2]],F(1,16)),expected={'matte':[[0,.5,1]],'raw_residual':0})
add('KEY-06A',k.foreground_regularized,([.5]*3,.5,[0]*3,[1]*3,.25),expected=[1]*3)
add('KEY-07A',partial(k.morphology,mode='erode'),([[0,1],[1,1]],1),expected=[[0,0],[0,0]])
add('KEY-07B',partial(k.morphology,mode='dilate'),([[0,1],[0,0]],1),expected=[[1,1],[1,1]])
add('KEY-07C',k.fill_binary_holes,([[1,1,1],[1,0,1],[1,1,1]],),expected=[[1]*3]*3)
add('KEY-07D',k.guided_refine_gray,([[.5,.5]],[[0,1]],1,.125),expected=[[.5,.5]])
add('KEY-08A',k.green_excess,([.25,1,.5,.75],1,1),expected=[.25,.5,.5,.75])
add('KEY-08B',k.directional_despill,([.5,.5,.5,.75],[0,1,0],1,1,[F(1,4),F(1,2),F(1,4)]),expected=[.5,.5,.5,.75])
add('KEY-09A',k.edge_extend_nearest,([[[1,0,0,1],[0,0,1,0]]],1,1),expected=[[[1,0,0,1],[1,0,0,0]]])
