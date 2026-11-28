//! dfbridge: DeepFilterNet3 for the Shizumaru plug-in.
//!
//! The network runs once per hop on the mid (average) channel; its ERB gains
//! and deep-filter coefficients are then applied to every channel's own
//! spectrum, so stereo costs about the same as mono and the image is kept.
//! A voice guard keeps harmonic, clearly-above-noise bins (screams, shouts)
//! from being pulled below a spectral-subtraction floor. Non-48 kHz hosts are
//! resampled inside the bridge. The C API is at the bottom.

mod resample;

use df::tract::{DfParams, DfTract, RuntimeParams};
use df::{Complex32, DFState};
use resample::Resampler;
use std::collections::VecDeque;

static MODEL_LL: &[u8] = include_bytes!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../build-deps/DeepFilterNet/models/DeepFilterNet3_ll_onnx.tar.gz"
));
static MODEL_STD: &[u8] = include_bytes!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../build-deps/DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz"
));

pub const SR: usize = 48000;

// ---------------------------------------------------------------- voice guard
struct Guard {
    nf: usize,
    s: Vec<f32>,        // smoothed mid power
    sub_min: Vec<f32>,  // min of the current sub-window
    mins: Vec<Vec<f32>>, // finished sub-window minima
    sub_pos: usize,
    sub_idx: usize,
    noise: Vec<f32>,
    frames: usize,
    logsnr: Vec<f32>,
    pub weight: Vec<f32>, // per-bin guard weight 0..1 for the current frame
    pub voicing: f32,
    pub f0: f32,
    broad: f32,
    ov: f32,
    top_hz: f32,
    top_bin: usize,
    snr_lo: f32,
    keep_hi: f32,
    lvl_lo: f32,
}

fn envf(k: &str, d: f32) -> f32 {
    std::env::var(k).ok().and_then(|v| v.parse().ok()).unwrap_or(d)
}

const SUB_LEN: usize = 12;
const SUB_N: usize = 8;

impl Guard {
    fn new(nf: usize) -> Self {
        Self {
            nf,
            s: vec![0.0; nf],
            sub_min: vec![f32::MAX; nf],
            mins: vec![vec![f32::MAX; nf]; SUB_N],
            sub_pos: 0,
            sub_idx: 0,
            noise: vec![0.0; nf],
            frames: 0,
            logsnr: vec![0.0; nf],
            weight: vec![0.0; nf],
            voicing: 0.0,
            f0: 0.0,
            broad: envf("DFB_BROAD", 1.0),
            top_bin: 0,
            snr_lo: envf("DFB_SNRLO", 6.0),
            keep_hi: envf("DFB_KEEP", 0.5),
            lvl_lo: envf("DFB_LVL", -9.0),
            ov: envf("DFB_OV", 2.0),
            top_hz: envf("DFB_TOP", 8000.0),
        }
    }

    fn reset(&mut self) {
        let (b, o, t, tb, sl, kh, ll) =
            (self.broad, self.ov, self.top_hz, self.top_bin, self.snr_lo, self.keep_hi, self.lvl_lo);
        *self = Guard::new(self.nf);
        self.broad = b;
        self.ov = o;
        self.top_hz = t;
        self.top_bin = tb;
        self.snr_lo = sl;
        self.keep_hi = kh;
        self.lvl_lo = ll;
    }

    fn val(&self, bin: f32) -> f32 {
        let i = bin as usize;
        if i + 1 >= self.nf {
            return 0.0;
        }
        let f = bin - i as f32;
        self.logsnr[i] + (self.logsnr[i + 1] - self.logsnr[i]) * f
    }

    /// Update the noise floor from the mid spectrum and find a harmonic voice.
    fn update(&mut self, mid: &[Complex32], bin_hz: f32) {
        let nf = self.nf;
        for k in 0..nf {
            let p = mid[k].norm_sqr();
            self.s[k] = if self.frames == 0 { p } else { 0.6 * self.s[k] + 0.4 * p };
            self.sub_min[k] = self.sub_min[k].min(self.s[k]);
        }
        self.frames += 1;
        self.sub_pos += 1;
        if self.sub_pos >= SUB_LEN {
            self.sub_pos = 0;
            self.mins[self.sub_idx].copy_from_slice(&self.sub_min);
            self.sub_idx = (self.sub_idx + 1) % SUB_N;
            self.sub_min.fill(f32::MAX);
        }
        for k in 0..nf {
            let mut m = self.sub_min[k];
            for w in &self.mins {
                m = m.min(w[k]);
            }
            self.noise[k] = 1.5 * m + 1e-12;
            let snr = (mid[k].norm_sqr() / self.noise[k]).max(1.0);
            self.logsnr[k] = snr.ln().min(8.0);
        }
        // harmonic salience over f0 = 80..1100 Hz (screams sit high)
        let mut best = 0f32;
        let mut best_f0 = 0f32;
        let mut f0 = 80f32;
        while f0 <= 1100.0 {
            let fb = f0 / bin_hz;
            let mut sum = 0f32;
            let mut nh = 0;
            let mut h = 1f32;
            while h * f0 < 6000.0 && nh < 20 {
                let c = h * fb;
                sum += self.val(c) - 0.5 * (self.val(c - 0.5 * fb) + self.val(c + 0.5 * fb));
                nh += 1;
                h += 1.0;
            }
            let sal = if nh > 0 { sum / nh as f32 } else { 0.0 };
            if sal > best {
                best = sal;
                best_f0 = f0;
            }
            f0 *= 1.006;
        }
        // e^1.2 ~ 5 dB average harmonic prominence = clearly voiced
        self.voicing = ((best - 0.8) / 1.2).clamp(0.0, 1.0);
        self.f0 = best_f0;
        self.weight.fill(0.0);
        if self.voicing > 0.0 {
            let fb = best_f0 / bin_hz;
            let top = ((self.top_hz / bin_hz) as usize).min(nf - 1);
            // loud voiced frames: hold every bin below `top`, not just the harmonics
            let wb = self.broad * ((self.voicing - 0.3) / 0.4).clamp(0.0, 1.0);
            for k in 1..top {
                self.weight[k] = wb;
            }
            let mut h = 1f32;
            while h * fb < top as f32 {
                let c = h * fb;
                let lo = (c - 1.0).ceil().max(0.0) as usize;
                let hi = ((c + 1.0).floor() as usize).min(nf - 1);
                for k in lo..=hi {
                    let w = (1.0 - (k as f32 - c).abs()).max(0.0);
                    self.weight[k] = self.weight[k].max(w);
                }
                h += 1.0;
            }
        }
    }
}

// ------------------------------------------------------------- 48 kHz engine
struct Engine {
    tract: DfTract,
    nch: usize,
    hop: usize,
    nf: usize,
    nb_df: usize,
    df_order: usize,
    lookahead: usize,
    states: Vec<DFState>,
    rx: Vec<VecDeque<Vec<Complex32>>>,
    ry: Vec<VecDeque<Vec<Complex32>>>,
    cur: Vec<Complex32>,
    enh: Vec<Complex32>,
    enh_ch: Vec<Vec<Complex32>>,
    pub guard_level: f32,
    speech_lvl: f32,
    mid: Vec<Complex32>,
    guard: Guard,
    skip_counter: usize,
    pub guard_amt: f32,
    pub atten_lim: Option<f32>,
    pub post_filter_beta: f32,
    pub gains: Vec<f32>, // per-bin |Y|/|X| of the last frame (meter)
    pub lsnr: f32,
}

impl Engine {
    fn new(model: i32, nch: usize) -> Option<Self> {
        let bytes = if model == 1 { MODEL_STD } else { MODEL_LL };
        let dfp = DfParams::from_bytes(bytes).ok()?;
        // same thresholds as the reference deep-filter tool
        let rp = RuntimeParams::default_with_ch(1).with_thresholds(-15.0, 35.0, 35.0);
        let tract = DfTract::new(dfp, &rp).ok()?;
        let nf = tract.n_freqs;
        let hop = tract.hop_size;
        let lookahead = tract.lookahead;
        let df_order = tract.df_order;
        let nx = df_order.max(lookahead);
        let ny = df_order + tract.conv_lookahead;
        let mut states = Vec::new();
        for _ in 0..nch {
            let mut s = DFState::new(tract.sr, tract.fft_size, hop, tract.nb_erb, tract.min_nb_erb_freqs);
            s.init_norm_states(tract.nb_df);
            states.push(s);
        }
        let z = vec![Complex32::default(); nf];
        Some(Self {
            nb_df: tract.nb_df,
            tract,
            nch,
            hop,
            nf,
            df_order,
            lookahead,
            states,
            rx: (0..nch).map(|_| (0..nx).map(|_| z.clone()).collect()).collect(),
            ry: (0..nch).map(|_| (0..ny).map(|_| z.clone()).collect()).collect(),
            cur: z.clone(),
            enh: z.clone(),
            enh_ch: (0..nch).map(|_| z.clone()).collect(),
            guard_level: 0.0,
            speech_lvl: 0.0,
            mid: z.clone(),
            guard: {
                let mut g = Guard::new(nf);
                g.top_bin = ((g.top_hz / (SR as f32 / (2 * (nf - 1)) as f32)) as usize).min(nf - 1);
                g
            },
            skip_counter: 0,
            guard_amt: 1.0,
            atten_lim: None,
            post_filter_beta: 0.0,
            gains: vec![1.0; nf],
            lsnr: 0.0,
        })
    }

    /// Algorithmic delay of one hop in, one hop out (samples at 48 kHz).
    fn delay(&self) -> usize {
        self.tract.fft_size - self.hop + self.lookahead * self.hop
    }

    fn reset(&mut self) {
        let _ = self.tract.init();
        for s in &mut self.states {
            s.reset();
        }
        for q in self.rx.iter_mut().chain(self.ry.iter_mut()) {
            for f in q.iter_mut() {
                f.fill(Complex32::default());
            }
        }
        self.guard.reset();
        self.speech_lvl = 0.0;
        self.skip_counter = 0;
    }

    /// One hop: `input[c]` and `output[c]` hold `hop` samples.
    fn process_hop(&mut self, input: &[&[f32]], output: &mut [&mut [f32]]) {
        let nch = self.nch;
        let hop = self.hop;
        // mirror DfTract::process: long digital silence -> zeros, skip the network
        let mut e = 0f32;
        for c in 0..nch {
            for &x in input[c] {
                e += x * x;
            }
        }
        let rms = e / (nch * hop) as f32;
        if rms < 1e-7 {
            self.skip_counter += 1;
        } else {
            self.skip_counter = 0;
        }
        if self.skip_counter > 5 {
            for o in output.iter_mut() {
                o.fill(0.0);
            }
            self.gains.fill(1.0);
            return;
        }

        self.mid.fill(Complex32::default());
        let inv = 1.0 / nch as f32;
        for c in 0..nch {
            self.states[c].analysis(input[c], &mut self.cur);
            for k in 0..self.nf {
                self.mid[k] += self.cur[k] * inv;
            }
            let mut f = self.rx[c].pop_front().unwrap();
            f.copy_from_slice(&self.cur);
            self.rx[c].push_back(f);
            let mut f = self.ry[c].pop_front().unwrap();
            f.copy_from_slice(&self.cur);
            self.ry[c].push_back(f);
        }
        if let Some(l) = self.atten_lim {
            if l >= 1.0 {
                // network off: plain delayed input
                for c in 0..nch {
                    let i = self.lookahead.max(self.df_order) - self.lookahead - 1;
                    self.enh.copy_from_slice(&self.rx[c][i]);
                    self.states[c].synthesis(&mut self.enh, output[c]);
                }
                self.gains.fill(1.0);
                return;
            }
        }
        // network on the mid spectrum
        if let Ok(buf) = self.tract.spec_buf.as_slice_mut::<f32>() {
            for k in 0..self.nf {
                buf[2 * k] = self.mid[k].re;
                buf[2 * k + 1] = self.mid[k].im;
            }
        }
        let (lsnr, gains, coefs) = match self.tract.process_raw() {
            Ok(r) => r,
            Err(_) => (0.0, None, None),
        };
        self.lsnr = lsnr;
        let (apply_erb, _, _) = self.tract.apply_stages(lsnr);
        let gains: Option<Vec<f32>> = gains.and_then(|g| g.as_slice::<f32>().ok().map(|s| s.to_vec()));
        let coefs: Option<Vec<f32>> = coefs.and_then(|g| g.as_slice::<f32>().ok().map(|s| s.to_vec()));
        if gains.is_some() {
            self.skip_counter = 0;
        } else {
            self.skip_counter += 1;
        }

        let ix = self.lookahead.max(self.df_order) - self.lookahead - 1;
        // guard works on the mid at the output frame's time
        {
            let mut tmp = std::mem::take(&mut self.mid);
            tmp.fill(Complex32::default());
            for c in 0..nch {
                for k in 0..self.nf {
                    tmp[k] += self.rx[c][ix][k] * inv;
                }
            }
            self.guard.update(&tmp, SR as f32 / self.tract.fft_size as f32);
            self.mid = tmp;
        }
        self.gains.fill(0.0);
        // pass 1: network output per channel, and how much signal it kept compared with
        // what a conservative stationary-noise subtraction says is there
        let mut e_enh = 0f32;
        let mut e_ss = 0f32;
        let mut e_noisy = 0f32;
        let mut e_noise = 0f32;
        let top = self.guard.top_bin;
        for c in 0..nch {
            let yi = self.df_order - 1;
            if let Some(g) = &gains {
                self.tract.df_states[0].apply_mask(&mut self.ry[c][yi], g);
            }
            let enh = &mut self.enh_ch[c];
            enh.copy_from_slice(&self.ry[c][yi]);
            if let Some(cf) = &coefs {
                // deep filtering of the lowest nb_df bins over df_order noisy frames
                for k in 0..self.nb_df {
                    let mut acc = Complex32::default();
                    for i in 0..self.df_order {
                        let o = (k * self.df_order + i) * 2;
                        acc += self.rx[c][i][k] * Complex32::new(cf[o], cf[o + 1]);
                    }
                    enh[k] = acc;
                }
            }
            let noisy = &self.rx[c][ix];
            if apply_erb && self.post_filter_beta > 0.0 {
                df::post_filter(noisy, enh, self.post_filter_beta);
            }
            for k in 1..top {
                let p = noisy[k].norm_sqr();
                let nn = self.guard.noise[k];
                e_noisy += p;
                e_noise += nn;
                e_ss += (p - self.guard.ov * nn).max(0.0);
                e_enh += enh[k].norm_sqr();
            }
        }
        // The guard only steps in when the frame is a clear, loud voice (harmonic, well
        // above the noise floor) AND the network threw most of it away: the signature of
        // DFN mistaking a scream or shout for noise. Normal speech is left to the network.
        let snr_db = 10.0 * ((e_noisy - e_noise).max(1e-20) / e_noise.max(1e-20)).log10();
        // guard_amt 1..2 ("strong voice protect") relaxes the three gates
        let relax = (self.guard_amt - 1.0).clamp(0.0, 1.0);
        let g_snr = ((snr_db - self.guard.snr_lo + 3.0 * relax) / 6.0).clamp(0.0, 1.0);
        let kept = if e_ss > 0.0 { e_enh / e_ss } else { 1.0 };
        let keep_hi = self.guard.keep_hi + 0.3 * relax;
        let g_rem = ((keep_hi - kept) / (keep_hi * 0.6)).clamp(0.0, 1.0);
        // ...and the frame is about as loud as speech the network itself accepted
        // (noise bursts in pauses sit well below that; screams sit at or above it)
        if kept >= 0.6 && snr_db > 6.0 {
            self.speech_lvl = self.speech_lvl.max(e_noisy);
        }
        self.speech_lvl *= 0.9986; // ~ -6 dB per 10 s
        let ref_lvl = if self.speech_lvl > e_noise * 31.6 { self.speech_lvl } else { e_noise * 31.6 };
        let rel_db = 10.0 * (e_noisy.max(1e-20) / ref_lvl.max(1e-20)).log10();
        let g_lvl = ((rel_db - self.guard.lvl_lo + 6.0 * relax) / 6.0).clamp(0.0, 1.0);
        let gv = self.guard.voicing * self.guard_amt.min(1.0) * g_snr * g_rem * g_lvl;
        self.guard_level = gv;
        // pass 2: guard floor, attenuation limit, synthesis
        for c in 0..nch {
            let noisy = &self.rx[c][ix];
            let enh = &mut self.enh_ch[c];
            if gv > 0.0 {
                for k in 0..self.nf {
                    let w = self.guard.weight[k];
                    if w <= 0.0 {
                        continue;
                    }
                    let p = noisy[k].norm_sqr();
                    if p <= 0.0 {
                        continue;
                    }
                    let fl = (1.0 - self.guard.ov * self.guard.noise[k] / p).max(0.0).sqrt() * w * gv;
                    if enh[k].norm_sqr() < fl * fl * p {
                        enh[k] = noisy[k] * fl;
                    }
                }
            }
            if let Some(lim) = self.atten_lim {
                for k in 0..self.nf {
                    enh[k] = enh[k] * (1.0 - lim) + noisy[k] * lim;
                }
            }
            for k in 0..self.nf {
                let n = noisy[k].norm();
                let r = if n > 1e-9 { (enh[k].norm() / n).min(1.0) } else { 1.0 };
                self.gains[k] += r * inv;
            }
            self.states[c].synthesis(enh, output[c]);
        }
    }
}

// --------------------------------------------------- host-rate streaming wrapper
const HOP: usize = 480;

/// Model delay at 48 kHz: STFT (fft - hop) + lookahead frames (from the models' config.ini).
pub fn model_delay(model: i32) -> usize {
    if model == 1 {
        480 + 2 * HOP // DeepFilterNet3: df_lookahead 2
    } else {
        480 // DeepFilterNet3 low latency: no lookahead
    }
}

fn needs_resampling(host_sr: f64) -> bool {
    (host_sr - SR as f64).abs() > 0.5
}

/// (output FIFO prefill, total latency) in host samples.
pub fn plan(model: i32, host_sr: f64) -> (usize, usize) {
    if !needs_resampling(host_sr) {
        return (HOP, HOP + model_delay(model));
    }
    let wu = Resampler::new(host_sr, SR as f64).half;
    let wd = Resampler::new(SR as f64, host_sr).half;
    let p = (((wd + HOP) as f64) * host_sr / SR as f64).ceil() as usize + wu + 2;
    let d = (model_delay(model) as f64 * host_sr / SR as f64).round() as usize;
    (p, p + d)
}
pub struct Dfb {
    eng: Engine,
    nch: usize,
    host_sr: f64,
    up: Vec<Resampler>,
    down: Vec<Resampler>,
    in48: Vec<Vec<f32>>,
    tmp: Vec<f32>,
    hop_out: Vec<Vec<f32>>,
    fifo: Vec<VecDeque<f32>>,
    prefill: usize,
    latency: usize,
    pub underruns: u64,
    in_copy: Vec<Vec<f32>>,
    broken: bool,
    delay_line: Vec<VecDeque<f32>>, // dry path used while broken
}

impl Dfb {
    pub fn new(model: i32, nch: usize, host_sr: f64) -> Option<Self> {
        let nch = nch.clamp(1, 8);
        let eng = Engine::new(model, nch)?;
        let hop = eng.hop;
        debug_assert_eq!(eng.delay(), model_delay(model));
        let resample = needs_resampling(host_sr);
        let (up, down) = if resample {
            (
                (0..nch).map(|_| Resampler::new(host_sr, SR as f64)).collect(),
                (0..nch).map(|_| Resampler::new(SR as f64, host_sr)).collect(),
            )
        } else {
            (Vec::new(), Vec::new())
        };
        let (prefill, latency) = plan(model, host_sr);
        let mut s = Self {
            eng,
            nch,
            host_sr,
            up,
            down,
            in48: (0..nch).map(|_| Vec::with_capacity(1 << 14)).collect(),
            tmp: Vec::with_capacity(1 << 14),
            hop_out: (0..nch).map(|_| vec![0.0; hop]).collect(),
            fifo: (0..nch).map(|_| VecDeque::with_capacity(1 << 15)).collect(),
            prefill,
            latency,
            underruns: 0,
            in_copy: (0..nch).map(|_| Vec::with_capacity(8192)).collect(),
            broken: false,
            delay_line: (0..nch).map(|_| VecDeque::with_capacity(1 << 15)).collect(),
        };
        s.reset();
        Some(s)
    }

    pub fn latency(&self) -> usize {
        self.latency
    }

    pub fn reset(&mut self) {
        self.eng.reset();
        for r in self.up.iter_mut().chain(self.down.iter_mut()) {
            r.reset();
        }
        for c in 0..self.nch {
            self.in48[c].clear();
            self.fifo[c].clear();
            self.fifo[c].extend(std::iter::repeat(0.0).take(self.prefill));
            self.delay_line[c].clear();
            self.delay_line[c].extend(std::iter::repeat(0.0).take(self.latency));
        }
    }

    fn push(&mut self, input: &[&[f32]]) {
        let hop = self.eng.hop;
        for c in 0..self.nch {
            if self.up.is_empty() {
                self.in48[c].extend_from_slice(input[c]);
            } else {
                self.tmp.clear();
                self.up[c].process(input[c], &mut self.tmp);
                self.in48[c].extend_from_slice(&self.tmp);
            }
        }
        let mut pos = 0;
        while self.in48[0].len() - pos >= hop {
            {
                let nch = self.nch;
                let in48 = &self.in48;
                let ins: [&[f32]; 8] = std::array::from_fn(|c| if c < nch { &in48[c][pos..pos + hop] } else { &[][..] });
                let mut it = self.hop_out.iter_mut();
                let mut outs: [&mut [f32]; 8] = std::array::from_fn(|_| match it.next() {
                    Some(v) => &mut v[..],
                    None => &mut [][..],
                });
                self.eng.process_hop(&ins[..nch], &mut outs[..nch]);
            }
            for c in 0..self.nch {
                if self.down.is_empty() {
                    self.fifo[c].extend(self.hop_out[c].iter().copied());
                } else {
                    self.tmp.clear();
                    self.down[c].process(&self.hop_out[c], &mut self.tmp);
                    self.fifo[c].extend(self.tmp.iter().copied());
                }
            }
            pos += hop;
        }
        if pos > 0 {
            for c in 0..self.nch {
                self.in48[c].drain(..pos);
            }
        }
    }

    pub fn process(&mut self, input: &[&[f32]], output: &mut [&mut [f32]], n: usize) {
        // the dry delay line always runs so a failure can fall back without a jump in time
        for c in 0..self.nch {
            self.delay_line[c].extend(input[c][..n].iter().copied());
        }
        if !self.broken {
            let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| self.push(input)));
            if r.is_err() {
                self.broken = true;
            }
        }
        for c in 0..self.nch {
            for i in 0..n {
                let d = self.delay_line[c].pop_front().unwrap_or(0.0);
                output[c][i] = if self.broken {
                    d
                } else {
                    match self.fifo[c].pop_front() {
                        Some(v) => v,
                        None => {
                            self.underruns += 1;
                            0.0
                        }
                    }
                };
            }
        }
    }

    pub fn set_params(&mut self, atten_db: f32, guard: f32, pf_beta: f32) {
        let a = atten_db.abs();
        self.eng.atten_lim = if a >= 100.0 {
            None
        } else if a < 0.01 {
            Some(1.0)
        } else {
            Some(10f32.powf(-a / 20.0))
        };
        self.eng.guard_amt = guard.clamp(0.0, 2.0);
        self.eng.post_filter_beta = pf_beta.max(0.0);
    }

    pub fn gains(&self) -> &[f32] {
        &self.eng.gains
    }
    pub fn n_freqs(&self) -> usize {
        self.eng.nf
    }
    pub fn host_sr(&self) -> f64 {
        self.host_sr
    }
    /// voicing (0..1), f0 and the model's local SNR of the last hop (diagnostics)
    pub fn debug_state(&self) -> (f32, f32, f32) {
        (self.eng.guard_level, self.eng.guard.f0, self.eng.lsnr)
    }
    pub fn is_broken(&self) -> bool {
        self.broken
    }
}

// ------------------------------------------------------------------ C API
#[no_mangle]
pub extern "C" fn dfb_create(model: i32, nch: i32, host_sr: f64) -> *mut Dfb {
    let r = std::panic::catch_unwind(|| Dfb::new(model, nch.max(1) as usize, host_sr));
    match r {
        Ok(Some(d)) => Box::into_raw(Box::new(d)),
        _ => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn dfb_free(st: *mut Dfb) {
    if !st.is_null() {
        unsafe { drop(Box::from_raw(st)) };
    }
}

#[no_mangle]
pub extern "C" fn dfb_latency(st: *const Dfb) -> i32 {
    if st.is_null() {
        return 0;
    }
    unsafe { (*st).latency() as i32 }
}

/// Latency in host samples that dfb_create(model, _, host_sr) will report (no model load).
#[no_mangle]
pub extern "C" fn dfb_latency_for(model: i32, host_sr: f64) -> i32 {
    plan(model, host_sr).1 as i32
}

#[no_mangle]
pub extern "C" fn dfb_reset(st: *mut Dfb) {
    if st.is_null() {
        return;
    }
    let s = unsafe { &mut *st };
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.reset()));
}

/// atten_db >= 100: no limit; guard 0..1 (1 = default), 1..2 = stronger; pf_beta 0 = no post-filter
#[no_mangle]
pub extern "C" fn dfb_set_params(st: *mut Dfb, atten_db: f32, guard: f32, pf_beta: f32) {
    if st.is_null() {
        return;
    }
    unsafe { (*st).set_params(atten_db, guard, pf_beta) };
}

/// Process n samples per channel (any n). in/out may alias.
#[no_mangle]
pub extern "C" fn dfb_process(st: *mut Dfb, input: *const *const f32, output: *const *mut f32, n: i32) {
    if st.is_null() || n <= 0 {
        return;
    }
    let s = unsafe { &mut *st };
    let n = n as usize;
    let nch = s.nch;
    // copy input first so in-place buffers are safe
    let mut ins = std::mem::take(&mut s.in_copy);
    for c in 0..nch {
        ins[c].clear();
        ins[c].extend_from_slice(unsafe { std::slice::from_raw_parts(*input.add(c), n) });
    }
    {
        let in_refs: [&[f32]; 8] = std::array::from_fn(|c| if c < nch { &ins[c][..] } else { &[][..] });
        let mut outs: [&mut [f32]; 8] = std::array::from_fn(|c| {
            if c < nch {
                unsafe { std::slice::from_raw_parts_mut(*output.add(c), n) }
            } else {
                &mut [][..]
            }
        });
        s.process(&in_refs[..nch], &mut outs[..nch], n);
    }
    s.in_copy = ins;
}

/// Copies min(n, n_freqs) per-bin gains (0..1) of the last frame; returns n_freqs.
#[no_mangle]
pub extern "C" fn dfb_gains(st: *const Dfb, out: *mut f32, n: i32) -> i32 {
    if st.is_null() {
        return 0;
    }
    let s = unsafe { &*st };
    let g = s.gains();
    if !out.is_null() && n > 0 {
        let m = (n as usize).min(g.len());
        unsafe { std::ptr::copy_nonoverlapping(g.as_ptr(), out, m) };
    }
    g.len() as i32
}

/// Bin spacing of dfb_gains in Hz (at 48 kHz).
#[no_mangle]
pub extern "C" fn dfb_bin_hz(st: *const Dfb) -> f32 {
    if st.is_null() {
        return 50.0;
    }
    let s = unsafe { &*st };
    (SR as f32 * 0.5) / (s.n_freqs() as f32 - 1.0)
}

#[no_mangle]
pub extern "C" fn dfb_is_broken(st: *const Dfb) -> i32 {
    if st.is_null() {
        return 1;
    }
    unsafe { (*st).is_broken() as i32 }
}
