import csv, glob, numpy as np
flicks=[]
for f in sorted(glob.glob("flick_*.csv")):
    rows=list(csv.DictReader(open(f)))
    t=np.array([float(r["t_ms"]) for r in rows])
    rx=np.array([float(r["rx"]) for r in rows]); ry=np.array([float(r["ry"]) for r in rows])
    flicks.append((t,rx,ry))
DT=3.0
def leaky(rx,tau):
    dec=np.exp(-DT/tau); e=np.zeros_like(rx); 
    for n in range(1,len(rx)): e[n]=dec*e[n-1]+(rx[n]-rx[n-1])
    return e
# joint fit: ry = K*e(tau) + per-flick constant ; sweep tau, maximize R2
best=None
for tau in np.arange(5,61,1.0):
    rows_e=[]; rows_y=[]; design=[]
    cols=len(flicks)
    E=[]; Y=[]; C=[]
    for fi,(t,rx,ry) in enumerate(flicks):
        e=leaky(rx,tau); E.append(e); Y.append(ry)
        c=np.zeros((len(ry),cols)); c[:,fi]=1.0; C.append(c)
    Eall=np.concatenate(E); Yall=np.concatenate(Y); Call=np.concatenate(C,axis=0)
    A=np.column_stack([Eall,Call])
    coef,res,rank,sv=np.linalg.lstsq(A,Yall,rcond=None)
    pred=A@coef; ss_res=np.sum((Yall-pred)**2); ss_tot=np.sum((Yall-Yall.mean())**2)
    R2=1-ss_res/ss_tot; K=coef[0]
    if best is None or R2>best[2]: best=(tau,K,R2,coef[1:])
tau,K,R2,consts=best
print(f"BEST FIT: tau = {tau:.0f} ms | K = {K:+.4f} | R2 = {R2*100:.1f}%")
print(f"per-flick hand/positional offsets (the part that ISN'T velocity):")
for fi,c in enumerate(consts): print(f"  flick_{fi}: {c:+.3f}")
print(f"\ninterpretation: velocity term explains {R2*100:.0f}% of Y once the")
print(f"per-flick holding offset is removed. tau~{tau:.0f}ms decay, K={K:+.3f}.")