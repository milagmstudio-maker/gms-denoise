import sys, numpy as np
sys.path.insert(0, "/home/claude/zerodenoise/prototype")
import cppeval as ce, realeval as re_
FS=48000
modes=sys.argv[1:]
res={}
for name in ["AirConditioner_1","tr_Office_1","tr_Cafe_1","tr_Traffic_1","hiss","Typing_1"]:
  for spk in range(2):
    clean=re_.speech_clip(spk); nz=re_.noise_clip(name,len(clean),spk)
    ps=np.mean(clean[np.abs(clean)>0.01]**2); nz=nz*np.sqrt(ps/10**(10/10)); mix=clean+nz
    fl=480; nf=len(clean)//fl
    ce_=np.array([np.sum(clean[k*fl:(k+1)*fl]**2) for k in range(nf)])
    act=ce_>ce_.max()*1e-6
    # distance since last speech frame
    since=np.full(nf,1e9); last=-1e9
    for k in range(nf):
        if act[k]: last=k
        since[k]=k-last
    cats={"lead0-1s":(np.arange(nf)<100),"lead1-3s":(np.arange(nf)>=100)&(np.arange(nf)<300),
          "after0-200ms":(~act)&(since<=20)&(np.arange(nf)>=300),"gap>200ms":(~act)&(since>20)&(np.arange(nf)>=300),"speech":act}
    for m in modes:
        oc,on,om=ce.run_mode(m,clean,nz,mix)
        ei=np.array([np.sum(nz[k*fl:(k+1)*fl]**2) for k in range(nf)])
        eo=np.array([np.sum(on[k*fl:(k+1)*fl]**2) for k in range(nf)])
        for c,msk in cats.items():
            res.setdefault((m,c),[[],[]]); res[(m,c)][0].append(eo[msk].sum()); res[(m,c)][1].append(ei[msk].sum())
        for c,msk in cats.items():
            res.setdefault((m,name,c),[0,0]); res[(m,name,c)][0]+=eo[msk].sum(); res[(m,name,c)][1]+=ei[msk].sum()
for m in modes:
  print(m, "  ".join(f"{c} {10*np.log10(sum(res[(m,c)][0])/sum(res[(m,c)][1])):6.1f}" for c in ["lead0-1s","lead1-3s","after0-200ms","gap>200ms","speech"]))
  for name in ["AirConditioner_1","tr_Office_1","tr_Cafe_1","tr_Traffic_1","hiss","Typing_1"]:
    print("   ",f"{name:17s}","  ".join(f"{c} {10*np.log10(res[(m,name,c)][0]/res[(m,name,c)][1]):6.1f}" for c in ["lead1-3s","after0-200ms","gap>200ms","speech"]))
