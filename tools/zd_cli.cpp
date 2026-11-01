// Offline runner: zd_cli in.wav out.wav [amount] [block]
// Processes in random-ish block sizes to exercise block-size independence.
#include "../dsp/ZeroDenoiser.h"
#include "wavio.h"
#include <chrono>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc < 3) { std::cerr << "usage: zd_cli in.wav out.wav [amount] [block]\n"; return 1; }
    wav::Audio a;
    if (!wav::read(argv[1], a)) { std::cerr << "cannot read " << argv[1] << "\n"; return 1; }
    zd::ZeroDenoiser d;
    const int nch = (int) a.ch.size();
    d.prepare(a.sr, nch);
    zd::DenoiserParams p;
    if (argc > 3) p.amount = std::atof(argv[3]);
    if (std::getenv("ZD_NOFAST")) p.onsetPath = false;
    if (std::getenv("ZD_AI")) p.ai = true;
    if (std::getenv("ZD_AI_PURE")) { p.aiHissAssist = false; p.aiVoiceGuard = false; }
    if (std::getenv("ZD_NOGUARD")) p.aiVoiceGuard = false;
    if (std::getenv("ZD_ACLO")) d.setVoicingBand(std::atof(std::getenv("ZD_ACLO")));
    if (std::getenv("ZD_TAIL")) d.dbgTail = std::atof(std::getenv("ZD_TAIL"));
    if (std::getenv("ZD_NOCOMB")) d.dbgComb = false;
    if (std::getenv("ZD_NOFLOOR")) d.dbgFloor = false;
    if (std::getenv("ZD_FBIAS")) d.floorBias = std::atof(std::getenv("ZD_FBIAS"));
    if (std::getenv("ZD_NOHOLD")) d.dbgNoHold = true;
    if (std::getenv("ZD_GSAT")) d.dbgSat = std::atof(std::getenv("ZD_GSAT"));
    if (std::getenv("ZD_GONSET")) d.dbgOnset = true;
    if (std::getenv("ZD_VREL")) d.setVoicingRelease(std::atof(std::getenv("ZD_VREL")));
    if (std::getenv("ZD_NOONSET")) d.dbgOnset = false;
    if (std::getenv("ZD_RLO")) d.setResidThresholds(std::atof(std::getenv("ZD_RLO")), std::atof(std::getenv("ZD_RHI")));
    if (std::getenv("ZD_VLO")) d.setVoicingThresholds(std::atof(std::getenv("ZD_VLO")), std::atof(std::getenv("ZD_VHI")));
    d.setParams(p);
    if (std::getenv("ZD_LOOKAHEAD")) d.setLookahead(std::atoi(std::getenv("ZD_LOOKAHEAD")));
    const int block = argc > 4 ? std::atoi(argv[4]) : 0; // 0 = varying
    const size_t n = a.ch[0].size();
    std::vector<float*> ptr(nch);
    unsigned seed = 1;
    FILE* dump = std::getenv("ZD_DUMP") ? std::fopen(std::getenv("ZD_DUMP"), "wb") : nullptr;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t pos = 0; pos < n;) {
        int bs = block;
        if (bs <= 0) { seed = seed * 1103515245u + 12345u; bs = 1 + (seed >> 16) % 700; }
        bs = (int) std::min<size_t>(bs, n - pos);
        for (int c = 0; c < nch; ++c) ptr[c] = a.ch[c].data() + pos;
        if (dump) {
            for (int i = 0; i < bs; ++i) {
                for (int c = 0; c < nch; ++c) ptr[c] = a.ch[c].data() + pos + i;
                d.process(ptr.data(), nch, 1);
                if ((pos + i + 1) % 64 == 0) {
                    if (std::getenv("ZD_VDUMP")) { float v[3] = { (float) d.getRawVoicing(), (float) d.getResid(), (float) d.getVoicing() }; std::fwrite(v, 4, 3, dump); }
                    else std::fwrite(d.currentGain().data(), 4, d.currentGain().size(), dump);
                }
            }
        } else
        d.process(ptr.data(), nch, bs);
        pos += bs;
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double audioSec = (double) n / a.sr;
    std::cerr << "processed " << audioSec << " s audio (" << nch << " ch) in " << sec
              << " s  -> " << 100.0 * sec / audioSec << " % of one core\n";
    if (dump) std::fclose(dump);
    wav::writeFloat(argv[2], a);
    return 0;
}
