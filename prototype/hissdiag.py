import sys, numpy as np
sys.path.insert(0, "/home/claude/zerodenoise/prototype")
import cppeval as ce, realeval as re_
from scipy.signal import stft
FS=48000
modes=sys.argv[1:]
regions=["lead0-1s","lead1-3s","speech","after0-300ms","gap"]
bands=[(0,2000),(2000,6000),(6000,24000)]
acc={}
for spk in range(4):
    clean=re_.speech_clip(spk); nz=re_.noise_clip("hiss",len(clean),spk)
    ps=np.mean(clean[np.abs(clean)>0.01]**2); nz=nz*np.sqrt(ps/10); mix=clean+nz
    fl=480; nf=len(clean)//fl
    ce_=np.array([np.sum(clean[k*fl:(k+1)*fl]**2) for k in range(nf)]); act=ce_>ce_.max()*1e-6
    since=np.full(nf,1e9); last=-1e9
    for k in range(nf):
        if act[k]: last=k
        since[k]=k-last
    idx=np.arange(nf)
    R={"lead0-1s":idx<100,"lead1-3s":(idx>=100)&(idx<300),"speech":act,"after0-300ms":(~act)&(since<=30)&(idx>=300),"gap":(~act)&(since>30)&(idx>=300)}
    f,t,Zi=stft(nz,FS,nperseg=960,noverlap=480)
    for m in modes:
        oc,on,om=ce.run_mode(m,clean,nz,mix)
        _,_,Zo=stft(on,FS,nperseg=960,noverlap=480)
        nfr=min(Zi.shape[1],nf)
        for r,msk in R.items():
            mk=msk[:nfr]
            for b in bands:
                sel=(f>=b[0])&(f<b[1])
                a=acc.setdefault((m,r,b),[0,0])
                a[0]+=np.sum(np.abs(Zo[sel][:, :nfr][:,mk])**2); a[1]+=np.sum(np.abs(Zi[sel][:, :nfr][:,mk])**2)
for m in modes:
    print(m)
    for r in regions:
        print(f"   {r:13s}"+"  ".join(f"{b[0]//1000}-{b[1]//1000}k {10*np.log10(acc[(m,r,b)][0]/acc[(m,r,b)][1]):6.1f}" for b in bands))
