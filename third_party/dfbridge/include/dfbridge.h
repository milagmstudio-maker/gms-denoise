// C API of third_party/dfbridge (DeepFilterNet3 for Shizumaru). See src/lib.rs.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Dfb Dfb;
// model 0 = DeepFilterNet3 low latency, 1 = DeepFilterNet3. Loads the network (slow: do not call on the audio thread).
Dfb* dfb_create(int model, int nch, double host_sr);
void dfb_free(Dfb*);
int dfb_latency(const Dfb*);
int dfb_latency_for(int model, double host_sr);
void dfb_reset(Dfb*);
// atten_db >= 100: no limit; guard 0..1 (1 = default), 1..2 = stronger; pf_beta 0 = no post-filter
void dfb_set_params(Dfb*, float atten_db, float guard, float pf_beta);
void dfb_process(Dfb*, const float* const* in, float* const* out, int n);
int dfb_gains(const Dfb*, float* out, int n);
float dfb_bin_hz(const Dfb*);
int dfb_is_broken(const Dfb*);
#ifdef __cplusplus
}
#endif
