#!/usr/bin/env python3
"""
check_rate.py - is the X->Y return bulge rate-dependent (snapback) or positional?

Reads a combined capture that contains BOTH a slow and a fast left-and-back.
Classifies every sample by instantaneous stick speed (|d rx| between frames),
then fits the RETURN-stroke bulge cY(rx) separately for slow vs fast samples.

  return bulge shrinks when slow  -> rate-dependent -> snapback -> temporal fix
  return bulge same slow vs fast  -> positional      -> single LUT is the ceiling

Usage: python3 check_rate.py bulge2.ndjson [--thresh 0.03]
"""
import json, sys

def load(path):
    pts=[]
    for line in open(path):
        line=line.strip()
        if not line: continue
        d=json.loads(line)
        if "rx" in d and "ry" in d:
            pts.append((float(d["rx"]), float(d["ry"])))
    return pts

def median(v):
    v=sorted(v); n=len(v)
    return None if n==0 else (v[n//2] if n%2 else 0.5*(v[n//2-1]+v[n//2]))

def otsu(vals, nb=64):
    lo,hi=min(vals),max(vals)
    if hi<=lo: return lo
    hist=[0]*nb
    for v in vals:
        hist[min(nb-1,int((v-lo)/(hi-lo)*nb))]+=1
    tot=len(vals); best=lo; bestvar=-1; w0=0; s0=0
    s_all=sum(i*hist[i] for i in range(nb))
    for i in range(nb):
        w0+=hist[i]; 
        if w0==0: continue
        w1=tot-w0
        if w1==0: break
        s0+=i*hist[i]
        m0=s0/w0; m1=(s_all-s0)/w1
        var=w0*w1*(m0-m1)**2
        if var>bestvar: bestvar=var; best=lo+(i+0.5)/nb*(hi-lo)
    return best

def fit(pts, NX):
    step=2.0/(NX-1); bins={i:[] for i in range(NX)}
    for rx,ry in pts:
        i=max(0,min(NX-1,int(round((rx+1.0)/step)))); bins[i].append(ry)
    cY=[median(bins[i]) for i in range(NX)]
    return cY

def main():
    path=sys.argv[1] if len(sys.argv)>1 else "bulge2.ndjson"
    thresh=None
    if "--thresh" in sys.argv: thresh=float(sys.argv[sys.argv.index("--thresh")+1])
    pts=load(path); NX=33; step=2.0/(NX-1)

    # per-sample speed + direction (samples ~uniform in time)
    speed=[]; rec=[]
    for k in range(1,len(pts)):
        dx=pts[k][0]-pts[k-1][0]
        speed.append(abs(dx)); rec.append((pts[k][0],pts[k][1],dx))
    moving=[s for s in speed if s>0.002]
    if thresh is None: thresh=otsu(moving)

    slow_ret=[]; fast_ret=[]; slow_push=[]; fast_push=[]
    for (rx,ry,dx),s in zip(rec,speed):
        if s<=0.002: continue
        ret = dx>0       # rx rising = returning toward center
        if ret:
            (fast_ret if s>thresh else slow_ret).append((rx,ry))
        else:
            (fast_push if s>thresh else slow_push).append((rx,ry))

    sp=sorted(moving); pct=lambda q: sp[int(q*(len(sp)-1))]
    print("="*64)
    print("  RATE-DEPENDENCE CHECK (return stroke = the bulgy one)")
    print("="*64)
    print(f"{len(pts)} pts | speed |drx| p10={pct(.1):.4f} p50={pct(.5):.4f} p90={pct(.9):.4f}")
    print(f"slow/fast threshold = {thresh:.4f}  (override with --thresh)")
    print(f"return: {len(slow_ret)} slow, {len(fast_ret)} fast | push: {len(slow_push)} slow, {len(fast_push)} fast\n")

    cyS=fit(slow_ret,NX); cyF=fit(fast_ret,NX); mid=(NX-1)//2
    print("  rx       slowReturn   fastReturn   fast-slow")
    peakS=peakF=0.0; maxgap=0.0
    for i in range(0,mid+1):
        rx=-1.0+i*step
        a=cyS[i] if cyS[i] is not None else 0.0
        b=cyF[i] if cyF[i] is not None else 0.0
        gap=b-a
        peakS=max(peakS,abs(a)); peakF=max(peakF,abs(b)); maxgap=max(maxgap,abs(gap))
        print(f"  {rx:+.3f}    {a:+.4f}      {b:+.4f}     {gap:+.4f}")
    print(f"\n  peak |slow return| = {peakS:.4f}")
    print(f"  peak |fast return| = {peakF:.4f}")
    print(f"  max slow/fast gap  = {maxgap:.4f}")
    print("-"*64)
    if peakF - peakS > 0.02 and peakS < 0.6*peakF:
        print("  VERDICT: RATE-DEPENDENT. Fast return bulges, slow return doesn't.")
        print("  -> snapback/lag. A position LUT can't fix it; a TEMPORAL (first-order")
        print("     lag) compensator can, and it won't hard-switch like the dual-LUT.")
    elif maxgap < 0.02:
        print("  VERDICT: POSITIONAL. Speed doesn't change it.")
        print("  -> the memoryless single LUT is the safe ceiling; manage the rest")
        print("     with deadzone. No temporal model needed.")
    else:
        print("  VERDICT: MIXED / inconclusive. Recapture cleaner slow & fast passes,")
        print("     or set --thresh manually using the percentiles above.")

if __name__=="__main__":
    main()