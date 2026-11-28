// dfb_wav in.wav out.wav [model 0=ll 1=std] [atten_db] [guard] [pf_beta] [block]
use dfbridge::Dfb;
fn main() {
    let a: Vec<String> = std::env::args().collect();
    let model: i32 = a.get(3).map(|s| s.parse().unwrap()).unwrap_or(0);
    let atten: f32 = a.get(4).map(|s| s.parse().unwrap()).unwrap_or(100.0);
    let guard: f32 = a.get(5).map(|s| s.parse().unwrap()).unwrap_or(1.0);
    let pf: f32 = a.get(6).map(|s| s.parse().unwrap()).unwrap_or(0.0);
    let block: usize = a.get(7).map(|s| s.parse().unwrap()).unwrap_or(256);
    let mut r = hound::WavReader::open(&a[1]).unwrap();
    let spec = r.spec();
    let nch = spec.channels as usize;
    let samples: Vec<f32> = match spec.sample_format {
        hound::SampleFormat::Float => r.samples::<f32>().map(|x| x.unwrap()).collect(),
        hound::SampleFormat::Int => {
            let sc = 1.0 / (1u32 << (spec.bits_per_sample - 1)) as f32;
            r.samples::<i32>().map(|x| x.unwrap() as f32 * sc).collect()
        }
    };
    let n = samples.len() / nch;
    let mut chans: Vec<Vec<f32>> = (0..nch).map(|c| (0..n).map(|i| samples[i * nch + c]).collect()).collect();
    let t0 = std::time::Instant::now();
    let mut d = Dfb::new(model, nch, spec.sample_rate as f64).expect("create");
    let tc = t0.elapsed().as_secs_f64();
    d.set_params(atten, guard, pf);
    let mut out: Vec<Vec<f32>> = vec![vec![0.0; n]; nch];
    let t1 = std::time::Instant::now();
    let mut i = 0;
    let mut times: Vec<f64> = Vec::new();
    let mut dbg = std::env::var("DFB_DEBUG").ok().map(|p| std::fs::File::create(p).unwrap());
    while i < n {
        let m = block.min(n - i);
        let ins: Vec<&[f32]> = chans.iter().map(|v| &v[i..i + m]).collect();
        let mut outs: Vec<&mut [f32]> = out.iter_mut().map(|v| &mut v[i..i + m]).collect();
        let tc0 = std::time::Instant::now();
        d.process(&ins, &mut outs, m);
        times.push(tc0.elapsed().as_secs_f64() * 1000.0);
        if let Some(f) = dbg.as_mut() { use std::io::Write; let (v, f0, l) = d.debug_state(); writeln!(f, "{} {} {}", v, f0, l).unwrap(); }
        i += m;
    }
    let tp = t1.elapsed().as_secs_f64();
    times.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let q = |f: f64| times[((times.len() as f64 - 1.0) * f) as usize];
    eprintln!("per-call ms: p50 {:.3} p99 {:.3} p99.9 {:.3} max {:.3}", q(0.5), q(0.99), q(0.999), q(1.0));
    chans.clear();
    eprintln!("latency {} create {:.2}s rtf {:.3} underruns {}", d.latency(), tc, tp / (n as f64 / spec.sample_rate as f64), d.underruns);
    let os = hound::WavSpec { channels: nch as u16, sample_rate: spec.sample_rate, bits_per_sample: 32, sample_format: hound::SampleFormat::Float };
    let mut w = hound::WavWriter::create(&a[2], os).unwrap();
    for i in 0..n { for c in 0..nch { w.write_sample(out[c][i]).unwrap(); } }
    w.finalize().unwrap();
    println!("{}", d.latency());
}
