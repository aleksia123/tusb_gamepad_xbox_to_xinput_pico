#!/usr/bin/env python3
"""
inspect_flick.py - look at the Y snapback transient of single fast flicks.

The aggregate regression failed (velocity explained only ~27%), so instead of
fitting another model we LOOK at the raw shape: for each isolated fast flick,
dump ry vs time (ms) and measure where/when the Y error peaks relative to the
moment the stick stops moving in X.

Key question this answers:
  * Y peaks AT peak |dx/dt| (during the move)   -> velocity transient
  * Y peaks AFTER x stops (during settle)        -> settling/overshoot (ringing)
  * decay time constant tau (ms)                 -> sets any time-based fix

Usage: python3 inspect_flick.py bulge_fast.ndjson
Requires the "t" field (ms). Writes flick_*.csv and a PNG if matplotlib present.
"""
import json, sys, statistics as st

def load(path):
    rows=[]
    for line in open(path):
        line=line.strip()
        if not line: continue
        d=json.loads(line)
        if "rx" in d and "ry" in d and d.get("t") is not None:
            rows.append((float(d["t"]), float(d["rx"]), float(d["ry"])))
    rows.sort(key=lambda r:r[0])
    return rows

def main():
    path=sys.argv[1] if len(sys.argv)>1 else "bulge_fast.ndjson"
    rows=load(path)
    if not rows:
        print("No timestamped rows. Re-capture with t enabled."); return
    ts=[r[0] for r in rows]
    dts=[ts[i]-ts[i-1] for i in range(1,len(ts)) if 0<ts[i]-ts[i-1]<100]
    dt_med=st.median(dts) if dts else 1.0
    print(f"{len(rows)} rows | median dt = {dt_med:.2f} ms ({1000/dt_med:.0f} Hz effective)")

    # per-sample x-speed; a "flick" = a contiguous run where |dx/dt| crosses high
    sp=[0.0]+[abs(rows[i][1]-rows[i-1][1]) for i in range(1,len(rows))]
    hi=sorted(sp)[int(0.97*len(sp))]            # fast threshold = 97th pct of speed
    thr=max(hi*0.5, 0.02)

    # find flick windows: from when speed first exceeds thr until x has been
    # essentially still (|dx|<0.01) for >=40 ms afterward
    flicks=[]; i=1; N=len(rows)
    while i<N:
        if sp[i]>thr:
            j0=i
            while j0>0 and sp[j0]>thr*0.25: j0-=1     # back up to motion start
            k=i; still=0
            while k<N-1:
                k+=1
                if sp[k]<0.01: still+= (rows[k][0]-rows[k-1][0])
                else: still=0
                if still>=40: break                    # 40 ms settled
            flicks.append((j0,k)); i=k+1
        else: i+=1

    print(f"isolated {len(flicks)} flick(s) (speed thr={thr:.3f})\n")
    summary=[]
    for n,(a,b) in enumerate(flicks[:8]):
        seg=rows[a:b+1]; t0=seg[0][0]
        # moment x stops: last index where speed>thr, then x settle time
        spk=max(range(a,b+1), key=lambda q:sp[q])          # peak speed index
        # time x effectively stops moving
        stop=spk
        for q in range(spk,b+1):
            if sp[q]<0.01: stop=q; break
        # Y peak (abs) index
        ypk=max(range(a,b+1), key=lambda q:abs(rows[q][2]))
        t_vpeak=rows[spk][0]-t0
        t_xstop=rows[stop][0]-t0
        t_ypeak=rows[ypk][0]-t0
        ymax=rows[ypk][2]
        # crude decay tau: time for |ry| to fall to 37% of its peak after ypeak
        tau=None
        for q in range(ypk,b+1):
            if abs(rows[q][2])<=0.37*abs(ymax)+1e-9:
                tau=rows[q][0]-rows[ypk][0]; break
        summary.append((t_vpeak,t_xstop,t_ypeak,ymax,tau))
        # write csv
        with open(f"flick_{n}.csv","w") as f:
            f.write("t_ms,rx,ry,speed\n")
            for q in range(a,b+1):
                f.write(f"{rows[q][0]-t0:.1f},{rows[q][1]:.4f},{rows[q][2]:.4f},{sp[q]:.4f}\n")
        print(f"flick {n}: Ypeak={ymax:+.3f} | t(vpeak)={t_vpeak:.0f}ms  "
              f"t(xstop)={t_xstop:.0f}ms  t(Ypeak)={t_ypeak:.0f}ms  "
              f"decay tau={'%.0fms'%tau if tau else 'n/a'}")

    if summary:
        # verdict: does Y peak during the move (near vpeak) or after x stops?
        after=sum(1 for s in summary if s[2] > s[1]+ (dt_med))   # ypeak after xstop
        during=len(summary)-after
        taus=[s[4] for s in summary if s[4]]
        print(f"\n  Y peaks DURING motion: {during}   |   Y peaks AFTER x stops: {after}")
        if taus: print(f"  median decay tau ~ {st.median(taus):.0f} ms")
        print("-"*58)
        if after>during:
            print("  SETTLING / OVERSHOOT transient: Y rings out after the stick stops.")
            print("  -> happens during decel, not while you're tracking. Options: a short")
            print("     post-stop settle-suppressor, or just mask it (it's off-target time).")
        else:
            print("  VELOCITY transient during the move, but NOT one-sample (it has a")
            print("     decay). A filtered/leaky lead with the tau above can fit it -")
            print("     not the instantaneous dx term we tried.")
        print("  CSVs written (flick_*.csv) - paste one and I'll read the exact shape.")

    # optional plot
    try:
        import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
        fig,ax=plt.subplots(figsize=(8,5),dpi=140); ax2=ax.twinx()
        for n,(a,b) in enumerate(flicks[:4]):
            seg=rows[a:b+1]; t0=seg[0][0]
            ax.plot([r[0]-t0 for r in seg],[r[2] for r in seg],label=f"ry flick{n}")
            ax2.plot([r[0]-t0 for r in seg],[r[1] for r in seg],ls=":",alpha=.5)
        ax.axhline(0,color="#888",lw=.6); ax.set_xlabel("ms"); ax.set_ylabel("ry (bulge)")
        ax2.set_ylabel("rx (dotted)"); ax.set_title("Fast-flick Y transient"); ax.legend(fontsize=8)
        plt.tight_layout(); plt.savefig("flick_transient.png",facecolor="white")
        print("  wrote flick_transient.png")
    except Exception as e:
        print(f"  (no plot: {e})")

if __name__=="__main__":
    main()