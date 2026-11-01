import sys, os, subprocess, numpy as np, soundfile as sf
sys.path.insert(0, "/home/claude/zerodenoise/prototype")
import realeval as re_, expreval as xe, cppeval as ce
FS=48000
mode = sys.argv[1] if len(sys.argv) > 1 else "ai"
extra = dict(ce.MODES[mode][0])
tot=0; rows=[]
for spk in ["p001","p002","p003","p004"]:
  for noise in ["tr_Cafe_1","hiss","tr_Office_1","AirConditioner_1"]:
    clean = xe.clip(spk, "scream"); nz = re_.noise_clip(noise, len(clean), int(spk[1:]))
    reg = xe.clip(spk, "regular"); ps = np.mean(reg[np.abs(reg) > 0.01] ** 2); nz *= np.sqrt(ps / 10)
    oc, on, om = ce.run_mode(mode, clean, nz, clean + nz)
    sf.write("/tmp/d.wav", (clean + nz).astype(np.float32), FS, subtype="FLOAT")
    subprocess.run(["/home/claude/zerodenoise/build/zd_cli", "/tmp/d.wav", "/tmp/o.wav", "0.7"], env=dict(os.environ, ZD_LOOKAHEAD="480", ZD_DUMP="/tmp/v.bin", ZD_VDUMP="1", **extra), check=True, capture_output=True)
    v = np.fromfile("/tmp/v.bin", np.float32).reshape(-1, 3)
    ec, eo = xe.frames_db(clean, 960), xe.frames_db(oc, 960)
    loud = ec > ec.max() - 25
    d = eo - ec
    bad = loud & (d < -3)
    print(f"{spk} {noise:16s} loud {loud.sum():4d} drop>3dB {bad.sum():3d} drop>6dB {(loud&(d<-6)).sum():3d} worst {d[loud].min():6.1f}")
    for i in np.where(bad)[0]:
        h = slice(i*15, i*15+15)
        rows.append((spk, noise, i*0.02, ec[i]-ec.max(), d[i], np.minimum(v[h,0],1).mean(), v[h,1].mean(), v[h,2].mean()))
import collections
print("\nbad frames: r, resid, guard distribution")
R=np.array([r[5:] for r in rows]) if rows else np.zeros((0,3))
if len(R):
    print("r pct", np.percentile(R[:,0],[10,50,90]).round(2), "resid pct", np.percentile(R[:,1],[10,50,90]).round(2), "guard pct", np.percentile(R[:,2],[10,50,90]).round(2))
    print("level rel max pct", np.percentile([r[3] for r in rows],[10,50,90]).round(1))
    for r in rows[:30]: print("  %s %-14s t=%5.2f lvl %5.1f drop %5.1f r %.2f resid %7.2f guard %.2f" % r)
