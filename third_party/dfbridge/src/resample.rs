//! Streaming windowed-sinc resampler with zero phase (output j is the input
//! signal evaluated at time j * fi / fo). Causality comes from the caller's
//! buffering: output j can be produced once input floor(j*fi/fo) + half exists.

pub struct Resampler {
    step: f64, // input samples per output sample
    pub half: usize,
    res: f64,
    table: Vec<f32>,
    hist: Vec<f32>,
    hist_start: u64,
    n_in: u64,
    j: u64,
}

fn bessel_i0(x: f64) -> f64 {
    let mut s = 1.0;
    let mut t = 1.0;
    let q = x * x / 4.0;
    for k in 1..50 {
        t *= q / ((k * k) as f64);
        s += t;
        if t < 1e-12 * s {
            break;
        }
    }
    s
}

impl Resampler {
    pub fn new(fi: f64, fo: f64) -> Self {
        let fc = (fo / fi).min(1.0) * 0.94; // cutoff relative to the input Nyquist
        let half = (24.0 / fc).ceil() as usize;
        let res = 512.0;
        let n = half * res as usize + 2;
        let beta = 8.0;
        let i0b = bessel_i0(beta);
        let mut table = vec![0f32; n];
        for (i, v) in table.iter_mut().enumerate() {
            let d = i as f64 / res;
            let r = d / half as f64;
            if r >= 1.0 {
                *v = 0.0;
                continue;
            }
            let x = std::f64::consts::PI * fc * d;
            let sinc = if x.abs() < 1e-9 { 1.0 } else { x.sin() / x };
            let w = bessel_i0(beta * (1.0 - r * r).sqrt()) / i0b;
            *v = (fc * sinc * w) as f32;
        }
        Self {
            step: fi / fo,
            half,
            res,
            table,
            hist: Vec::with_capacity(1 << 15),
            hist_start: 0,
            n_in: 0,
            j: 0,
        }
    }

    pub fn reset(&mut self) {
        self.hist.clear();
        self.hist_start = 0;
        self.n_in = 0;
        self.j = 0;
    }

    #[inline]
    fn h(&self, d: f64) -> f32 {
        let x = d.abs() * self.res;
        let i = x as usize;
        if i + 1 >= self.table.len() {
            return 0.0;
        }
        let f = (x - i as f64) as f32;
        self.table[i] + (self.table[i + 1] - self.table[i]) * f
    }

    /// Feed input, append every output sample that can be computed.
    pub fn process(&mut self, input: &[f32], out: &mut Vec<f32>) {
        self.hist.extend_from_slice(input);
        self.n_in += input.len() as u64;
        let half = self.half as i64;
        loop {
            let t = self.j as f64 * self.step;
            let c = t.floor() as i64;
            if c + half > self.n_in as i64 - 1 {
                break;
            }
            let lo = (c - half + 1).max(0);
            let hi = c + half;
            let mut s = 0f32;
            let base = self.hist_start as i64;
            for n in lo..=hi {
                s += self.hist[(n - base) as usize] * self.h(t - n as f64);
            }
            out.push(s);
            self.j += 1;
        }
        // drop history that no future output needs
        let need = ((self.j as f64 * self.step).floor() as i64 - half + 1).max(0) as u64;
        if need > self.hist_start + 4096 {
            let drop = (need - self.hist_start) as usize;
            self.hist.drain(..drop);
            self.hist_start = need;
        }
    }
}
