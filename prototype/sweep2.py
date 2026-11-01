import sys, numpy as np
import evaluate as ev
from dsp import *
clean=ev.load_clean(); ps=np.mean(clean[np.abs(clean)>0.01]**2)
cfgs=eval(sys.argv[1]); KS=sys.argv[2].split(',') if len(sys.argv)>2 else ['hiss','hvac']
for kind in KS:
  nz=ev.make_noise(kind,len(clean),[]); nz*=np.sqrt(ps/10**0.5)
  for c in cfgs:
    c=dict(c); L=c.pop('L',512)
    w=c.pop('win','asym'); est=GainEstimator(1024,64,w,**c)
    G=compute_gains(clean+nz,est); H=filters_from_gains(G,L)
    outs=[apply_tv_fir(s,H,64) for s in (clean,nz,clean+nz)]
    m=ev.metrics(clean,nz,*outs)
    print(kind,c,' '.join(f"{k}={v:.2f}" for k,v in m.items()),flush=True)
