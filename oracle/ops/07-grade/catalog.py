"""Member-to-reference mapping and small reproducible mathematical samples.

REFERENCES are reusable scalar/array callables. SAMPLES bind one representative
observation per member; they are not runtime integration tests.
"""
from functools import partial
from reference import point as p, curves as c, statistics as st, spatial as s
from reference import dither as d, color as col, extended as e
from reference.precision import F, frac, rn

REFERENCES={}
SAMPLES={}
EXPECTED={}


def add(member,fn,args=(),kwargs=None,expected=None):
    key='GRD-'+member
    REFERENCES[key]=fn
    SAMPLES[key]=(args,kwargs or {})
    if expected is not None:EXPECTED[key]=expected


def model_affine(v,controls,dtype=64):
    return [p.affine(x,*control,dtype=dtype) for x,control in zip(v,controls)]


def white_balance_temperature(rgb,temperature,tint=0,context=None,method='bradford',dtype=64):
    ctx=context or col.Context()
    white=col.temperature_white(temperature,tint)
    return col.white_balance(rgb,list(map(frac,white)),ctx.white,ctx,method=method,dtype=dtype)


ctx=col.Context()
rgb=[.25,.5,.75]
image=[[[.125]*3,[.25]*3],[[.5]*3,[.75]*3]]
feature=[[0,.25],[.75,1]]
selection=[[1,1],[1,1]]
table=[[[[r,g,b] for b in (0,1)] for g in (0,1)] for r in (0,1)]
zero_mesh=[[[0,0],[0,0]],[[0,0],[0,0]]]
z=[1,0,0]
add('01A',p.exposure,(.25,1),expected=.5)
add('01B',p.exposure,(.25,-1),expected=.125)
add('02A',p.affine,(.25,2,-.125),expected=.375)
add('02B',p.lgg,(-.25,0,2,1),expected=-.5)
add('03A',p.contrast_linear,(.5,2,.25),expected=.75)
add('03B',p.contrast_log,(.5,2,.25),expected=1.)
add('04A',p.levels,(-1,),expected=0.)
add('04B',p.levels_signed,(-.25,0,1,2),expected=-.5)
add('05A',c.linear,(.25,[0,1],[0,2]),expected=.5)
add('05B',c.pchip,(.25,[0,1],[0,2]),expected=.5)
for suffix,route in [('C','ratio'),('D','xyz_y'),('E','cielab_l'),('F','oklab_l')]:
    add('05'+suffix,partial(e.brightness_curve,route=route),(rgb,[0,1],[0,.75]))
add('06A',c.ramp,(.25,[0,1],[[0,0,0],[1,0,0]]),expected=[.25,0.,0.])
add('06B',e.gradient_image,(rgb,[0,1],[[0,0,0],[1,0,0]]))
add('07A',c.lut_trilinear,(rgb,table),expected=rgb)
add('07B',c.lut_tetrahedral,(rgb,table),expected=rgb)
add('08A',p.hsl,([0,.5,.5],.25,2,.1))
add('08B',p.hsv,([0,.5,.5],.25,2,.5),expected=[.25,1.,.25])
add('09A',p.lch,([.5,20,.25],.125,2,.5),expected=[.625,40.,.75])
add('09B',p.lch,([.5,.25,.25],.125,2,.5),expected=[.625,.5,.75])
add('10A',model_affine,([.5,20,-10],[[1,0],[2,0],[1,1]]),expected=[.5,40.,-9.])
add('10B',model_affine,([.5,.25,-.125],[[1,0],[2,0],[1,.125]]),expected=[.5,.5,0.])
add('11A',p.gain,(.25,2),expected=.5)
add('11B',col.white_balance,(rgb,ctx.white,ctx.white),expected=rgb)
add('11C',white_balance_temperature,(rgb,6500,0))
add('12A',p.monochrome,(rgb,[1,1,1]),{'normalize':True},expected=.5)
for suffix,route in [('B','ratio'),('C','cielab_l'),('D','oklab_l')]:
    add('12'+suffix,partial(ctx.monochrome,route=route),(rgb,))
add('13A',p.posterize,(.5,2),expected=0.)
add('14A',p.threshold,(.5,.5),expected=1)
add('14B',s.threshold_mean,(feature,0,0,1),expected=0)
add('14C',s.threshold_gaussian,(feature,1,1,1),expected=1)
add('14D',s.threshold_sauvola,(feature,1,1,1),expected=1)
add('15A',p.mixer,(rgb,[[0,0,1],[0,1,0],[1,0,0]],[0,0,0]),expected=list(reversed(rgb)))
add('16A',p.balance,(rgb,z,[[.125,0,0],[0,0,0],[0,0,0]]),expected=[.375,.5,.75])
add('16B',p.balance_opponent,([.5,.25,.125],z,[[.125,0],[0,0],[0,0]]),expected=[.5,.375,.125])
add('17A',p.invert,(2,),expected=-1.)
add('18A',p.zones,(.5,),expected=[0.,1.,0.])
add('18B',p.zone_grade,(.25,z,[[1,0],[0,0],[0,0]]),expected=.5)
add('19A',s.bilateral_base,([[.25,.25]],0,0),expected=-2.)
add('19B',p.local_grade,(.25,-2,[-2,1,1,0],[-1,0,1,2]),expected=.5)
add('19C',e.local,(image,),{'controls':[-2,1,.5,-.25],'detail_gain':.5,'radius':1})
add('19D',partial(e.local,controls=(-2,1,0,0)),(image,),{'detail_gain':.5,'radius':1})
add('20A',c.periodic,(F(3,2),[0,1],[0,1]),expected=.5)
add('20B',c.linear,(.25,[0,1],[1,2]),expected=1.25)
add('20C',p.hue_delta,(.25,1,.5),expected=.75)
add('20D',p.chroma_scale,([.5,3,4],2),expected=[.5,6.,8.])
add('20E',p.lightness_delta,([.5,.25,.125],.125),expected=[.625,.25,.125])
for suffix,route in [('F','ratio'),('H','xyz_y'),('I','cielab_l'),('J','oklab_l')]:
    add('20'+suffix,partial(ctx.replace,route=route),(rgb,.5))
add('20G',p.saturation_field,([.25,.75,.5],2),expected=[.25,1.,.5])
add('21A',partial(p.saturation,weights_y=ctx.m[1]),(rgb,0))
add('21B',p.chroma_scale,([.5,.25,.125],0),expected=[.5,0.,0.])
add('21C',p.vibrance,([.5,.125,0],1,.25),expected=[.5,.15625,0.])
add('22A',p.split_tone,([.5,0,0],z,[[.25,0],[0,0],[0,0]],[1,0,0]),expected=[.5,.25,0.])
add('22B',p.wheels,(rgb,z,[[[1,0],[0,0],[0,0]],[[0,0]]*3,[[0,0]]*3]),expected=[.5,.5,.75])
add('23A',p.select_region,([.5,.25,0],[0,.1,.2,0,.1,.4,.5,0,.25,.75,1]),expected=1.)
add('23B',p.selective,([.5,0,0],[1,1],[[0,.25,0],[0,.25,0]]),expected=[.5,.25,0.])
add('23C',e.neutral_region,([.5,0,0],[0,.1,0,.25,.75,1]),expected=1.)
add('24A',st.quantiles,([[0,10]],[[1,1]],[.5]),expected=([5.],2,2.))
add('24B',st.exposure_parameters,(.125,2,.25),expected=(1.,0))
add('24C',st.levels_parameters,([.25,.75],2),expected=([.25,.75,1.,0.,1.],0))
add('24D',e.auto_exposure,(image,selection),{'target':.25})
for suffix,route in [('E','ratio'),('F','xyz_y'),('G','cielab_l'),('H','oklab_l'),('I','channels')]:
    add('24'+suffix,partial(e.auto_levels,route=route),(image,selection),{'probabilities':(0,1)})
add('25A',st.histogram,(feature,selection,2),expected=([2,2],4,4.))
add('25B',st.cdf,([1,1],),expected=([0.,1.],0))
add('25C',st.hist_apply,(.75,[0,1],0),expected=1.)
add('25D',st.clahe_maps,(feature,selection),{'cell':(1,2),'bins':2})
maps,flags=st.clahe_maps(feature,selection,cell=(1,2),bins=2)
add('25E',st.clahe_apply,(0,0,.25,maps,flags,(2,2),(1,2)))
add('25F',e.equalize,(feature,selection),{'bins':2},expected=[[0.,0.],[1.,1.]])
add('25G',e.clahe,(feature,selection),{'bins':2,'cell':(1,2)})
add('26A',s.vignette,(0,0),expected=0.)
add('26B',e.vignette,(rgb,2,0),expected=[.125,.25,.375])
for suffix,fn in [('A',p.cdl_fwd),('B',p.cdl_rev),('C',p.cdl_fwd_no_clamp),('D',p.cdl_rev_no_clamp)]:
    add('27'+suffix,fn,(rgb,),expected=rgb)
add('28A',c.warper_ab,([.5,.25,.25],[0,1],[0,1],zero_mesh),expected=[.5,.25,.25])
add('28B',c.warper_hc,([.5,.25,2.5],[0,1],[0,1],zero_mesh),{'topology':'positive_jacobian'},expected=[.5,.25,2.5])
add('28C',c.warper_cl,([.5,.25,2.5],[0,1],[0,1],zero_mesh),expected=[.5,.25,2.5])
add('29A',d.bayer,(.5,0,0),expected=1.)
add('29B',d.noise,(.25,.1),expected=1.)
add('29C',d.floyd_steinberg,([[.5]*4],),{'scan':'raster'},expected=[[0.,1.,0.,1.]])
add('29D',d.ordered,(.5,0,0,[[.25,.75]]),expected=1.)
palette=[[0,0,0],[1,1,1]]
add('29E',e.palette_ordered,(rgb,palette,0,0,[[.5]]))
add('29F',e.palette_point,(rgb,palette,.5))
add('29G',e.palette_fs,(image,palette))
add('30A',d.halftone_dot,(.5,0,0))
add('30B',d.halftone_line,(.5,0,0))
add('30C',e.halftone_cmyk,([0,0,0,1],0,0,[{'method':'dot'}]*4),expected=[0.,0.,0.,1.])
add('30D',e.colorize,([.25],[[0,0,0]],[1,1,1]),expected=[.75,.75,.75])
