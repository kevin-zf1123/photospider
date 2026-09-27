"""Exact grouped color statistics; numeric oracle, not a metadata/Result codec."""
import math
from fractions import Fraction as Q
from exact import OracleError, atom, finite, integer, param, rn, sqrt_exact


def fit(samples, group_ids, weights, epsilon, dtype='float64'):
    if dtype not in ('float32','float64'): raise OracleError('sample dtype','TypeMismatch')
    if not isinstance(samples,list) or not samples or not samples[0]: raise OracleError('nonempty sample table required','TypeMismatch')
    n,c=len(samples),len(samples[0])
    if n>4096 or c>16: raise OracleError('manual statistics oracle cap','OracleCapacity')
    if any(len(row)!=c for row in samples) or len(group_ids)!=n or len(weights)!=n or len(epsilon)!=c:
        raise OracleError('sample/control shapes','TypeMismatch')
    eps=[param(x,'epsilon',0,True) for x in epsilon]
    groups={}
    for row,g,w in zip(samples,group_ids,weights):
        integer(g,'group_id')
        if not g: continue
        w=finite(atom(w,dtype),'weight')
        if w<0: raise OracleError('negative weight')
        x=[finite(atom(v,dtype),'color') for v in row] # includes zero weights
        groups.setdefault(g,[]).append((w,x))
    model=dict(ids=[],mean=[],cholesky=[])
    skipped=[]
    for g,rows in sorted(groups.items()):
        total=sum((w for w,x in rows),Q(0))
        if not total: skipped.append(g);continue
        mu=[sum((w*x[j] for w,x in rows),Q(0))/total for j in range(c)]
        cov=[[sum((w*(x[j]-mu[j])*(x[k]-mu[k]) for w,x in rows),Q(0))/total
              +(eps[j]**2 if j==k else 0) for k in range(c)] for j in range(c)]
        # Exact LDL^T avoids nested approximate square roots. Cholesky column j
        # equals unit-lower column j times sqrt(D[j]); each entry rounds once.
        lower=[[Q(int(j==k)) for k in range(c)] for j in range(c)]
        piv=[]
        for j in range(c):
            pivot=cov[j][j]-sum((lower[j][k]**2*piv[k] for k in range(j)),Q(0))
            if pivot<=0: raise OracleError('nonpositive exact pivot')
            piv.append(pivot)
            for i in range(j+1,c):
                lower[i][j]=(cov[i][j]-sum((lower[i][k]*lower[j][k]*piv[k] for k in range(j)),Q(0)))/pivot
        factor=[[rn(lower[j][k]*sqrt_exact(piv[k]),'float64') if k<=j else 0.
                 for k in range(c)] for j in range(c)]
        if any(factor[j][j]<=0 for j in range(c)): raise OracleError('unrepresentable positive factor','ArithmeticOverflow')
        model['ids'].append(g);model['mean'].append([rn(v,'float64') for v in mu]);model['cholesky'].append(factor)
    return dict(model=model,skipped_group_ids=skipped)


def fit_image(image, group_ids, weights, epsilon, channels, dtype='float64'):
    from reference import channel_indices
    if not image or not image[0] or not image[0][0]: raise OracleError('image shape','TypeMismatch')
    h,w=len(image[0]),len(image[0][0]);cs=channel_indices(channels,len(image))
    if len(group_ids)!=h or len(weights)!=h or any(len(row)!=w for row in group_ids+weights):
        raise OracleError('control shapes','TypeMismatch')
    if any(len(p)!=h or any(len(row)!=w for row in p) for p in image):raise OracleError('image planes','TypeMismatch')
    return fit([[image[c][y][x] for c in cs] for y in range(h) for x in range(w)],
               [g for row in group_ids for g in row],[v for row in weights for v in row],epsilon,dtype)


def apply(image, model, channels, inner, outer, curve='smoothstep', dtype='float64'):
    from reference import channel_indices, radii, radial, tensor
    image=tensor(image,dtype);cs=channel_indices(channels,len(image));c=len(cs)
    i,o=radii(inner,outer,curve)
    if any(k not in model for k in ('ids','mean','cholesky')):raise OracleError('incomplete model','TypeMismatch')
    ids=model['ids'];means=model['mean'];factors=model['cholesky']
    if len(ids)!=len(means) or len(ids)!=len(factors):raise OracleError('model shape','TypeMismatch')
    if ids!=sorted(set(ids)):raise OracleError('model group order')
    groups=[]
    for g,mu,l in zip(ids,means,factors):
        integer(g,'model group',1)
        if len(mu)!=c or len(l)!=c or any(len(row)!=c for row in l):raise OracleError('model dimensions','TypeMismatch')
        mu=[finite(atom(v,'float64')) for v in mu]
        l=[[atom(v,'float64') for v in row] for row in l]
        if any(float(l[j][k]) != 0 or math.copysign(1.,float(l[j][k])) < 0
               for j in range(c) for k in range(j+1,c)):
            raise OracleError('model upper triangle must be positive zero')
        l=[[finite(v) for v in row] for row in l]
        if any(l[j][j]<=0 for j in range(c)) or any(l[j][k]!=0 for j in range(c) for k in range(j+1,c)):
            raise OracleError('model is not positive lower triangular')
        groups.append((mu,l))
    out=[]
    for y in range(len(image[0])):
        row=[]
        for x in range(len(image[0][0])):
            if not groups:row.append(0.);continue
            point=[finite(image[j][y][x]) for j in cs]
            best=None
            for mu,l in groups:
                z=[]
                for j in range(c):z.append((point[j]-mu[j]-sum((l[j][k]*z[k] for k in range(j)),Q(0)))/l[j][j])
                score=sum((v*v for v in z),Q(0))
                best=score if best is None else min(best,score)
            # Shared monotone response: max responses equals response(min d).
            row.append(radial(sqrt_exact(best),i,o,curve,dtype))
        out.append(row)
    return out
