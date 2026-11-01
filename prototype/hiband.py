import numpy as np, sys
import realeval as R
R.extra_configs()
from realeval import CONFIGS, speech_clip, noise_clip
FS=48000
def bandloss(clean, filt, lo, hi):
    n=2048; h=512; w=np.hanning(n)
    fr=lambda x: np.abs(np.fft.rfft(np.lib.stride_tricks.sliding_window_view(x,n)[::h]*w,axis=1))**2
    C,F=fr(clean),fr(filt); e=C.sum(1); act=e>e.max()*1e-3
    k0,k1=int(lo/FS*n),int(hi/FS*n)
    return 10*np.log10(F[act,k0:k1].sum()/C[act,k0:k1].sum())
for cfg in sys.argv[1:]:
    rows=[]
    for spk in range(4):
        for snr in (5,10,20):
            c=speech_clip(spk); nz=noise_clip('hiss',len(c),spk); ps=np.mean(c[np.abs(c)>0.01]**2); nz*=np.sqrt(ps/10**(snr/10))
            oc,on,om=CONFIGS[cfg]((c,nz,c+nz))
            sk=2*FS
            rows.append([bandloss(c[sk:],oc[sk:],lo,hi) for lo,hi in ((100,1000),(1000,4000),(4000,8000),(8000,12000),(12000,20000))])
    r=np.array(rows).reshape(4,3,5).mean(0)
    for snr,row in zip((5,10,20),r): print(f"{cfg:10s} {snr:2d}dB  speech level change per band (dB) 0.1-1k/1-4k/4-8k/8-12k/12-20k:", ' '.join(f"{v:6.1f}" for v in row))
