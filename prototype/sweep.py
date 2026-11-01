import sys, numpy as np, soundfile as sf
import evaluate as ev
from dsp import *
clean=ev.load_clean()
ps=np.mean(clean[np.abs(clean)>0.01]**2)
cfgs=eval(sys.argv[1])
for kind in ['hiss','hvac']:
  nz=ev.make_noise(kind,len(clean),[]); nz*=np.sqrt(ps/10**0.5)
  for c in cfgs:
    est=GainEstimator(1024,256,'hann',**c)
    G=compute_gains(clean+nz,est)
    outs=[apply_stft(s,G,1024,256)[0] for s in (clean,nz,clean+nz)]
    m=ev.metrics(clean,nz,*outs)
    print(kind,c,{k:round(v,2) for k,v in m.items()})
