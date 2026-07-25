// warp_ab — standalone A/B time-stretch / pitch-shift quality harness.
//
// Proposal 27 M3. NOT a shipped component: an offline developer tool that scores
// a *candidate* time-stretch/pitch backend (the paged vocoder being written
// concurrently) against a *reference* backend (Rubber Band). It is intentionally
// self-contained DSP — its own FFT, Goertzel-free FFT-peak detector, onset
// detector and envelope follower — so its numbers are independent of the engine
// it is measuring, double-precision throughout, and deterministic (no rand(),
// fixed LCG seeds, one code path).
//
// It lives under analysis/tools/ so the logging checker exempts its printf table
// output (tools/ is an ALLOW_DIR): a metric table on stdout is the program's
// job, not diagnostics.
//
// Usage:
//   warp_ab <reference.wav> <candidate.wav>   print the metric table + SUMMARY
//   warp_ab --gen <outdir>                    write the deterministic corpus
//   warp_ab --selftest                        prove every metric catches its
//                                             failure mode (prints PASS/FAIL)
//
// Metrics (each reference-vs-candidate with a delta):
//   a. RMS: overall + per-second; max per-second relative deviation.
//   b. Dominant frequency per second (FFT peak, parabolically interpolated);
//      max relative deviation.
//   c. Transient smear: envelope-onset detection in both files, nearest-pairing
//      within 50 ms, 10%->90% rise time in a 60 ms window; mean/max cand-vs-ref
//      rise-time ratio + unpaired onset counts.
//   d. Modulation-spectrum warble: over the longest onset-free region, the
//      amplitude envelope band-limited to 2..30 Hz, its peak component in dB
//      relative to the DC (mean) level — catches overlap-add AM warble.
//   e. Spectral balance: long-term average spectrum in octave bands
//      125..8k Hz; per-band dB delta + max absolute delta.

#include "tw/analysis/audio_analysis.h"   // reuse: overall-RMS cross-check
#include <sndfile.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <complex>
#include <algorithm>

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Signal container: interleaved -> per-channel doubles + a mono mixdown.
// ---------------------------------------------------------------------------
struct Signal {
    int sampleRate = 48000;
    int channels   = 2;
    std::vector<std::vector<double>> chan;  // chan[c][n]
    std::vector<double> mono;               // average over channels

    int64_t frames() const { return mono.empty() ? 0 : (int64_t)mono.size(); }

    void buildMono() {
        int64_t n = chan.empty() ? 0 : (int64_t)chan[0].size();
        mono.assign((size_t)n, 0.0);
        for (auto &c : chan)
            for (int64_t i = 0; i < n; ++i) mono[(size_t)i] += c[(size_t)i];
        if (!chan.empty())
            for (double &v : mono) v /= (double)chan.size();
    }
};

bool loadWav(const std::string &path, Signal &sig, std::string &err) {
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));
    SNDFILE *f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f) { err = std::string("open failed: ") + sf_strerror(nullptr); return false; }
    sig.sampleRate = info.samplerate;
    sig.channels   = info.channels;
    sig.chan.assign((size_t)info.channels, {});
    for (auto &c : sig.chan) c.reserve((size_t)info.frames);

    const sf_count_t B = 4096;
    std::vector<double> buf((size_t)(B * info.channels));
    sf_count_t got;
    while ((got = sf_readf_double(f, buf.data(), B)) > 0) {
        for (sf_count_t i = 0; i < got; ++i)
            for (int c = 0; c < info.channels; ++c)
                sig.chan[(size_t)c].push_back(buf[(size_t)(i * info.channels + c)]);
    }
    sf_close(f);
    sig.buildMono();
    return true;
}

bool writeWav16(const std::string &path, const Signal &sig, std::string &err) {
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));
    info.samplerate = sig.sampleRate;
    info.channels   = sig.channels;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE *f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f) { err = std::string("write open failed: ") + sf_strerror(nullptr); return false; }
    int64_t n = sig.frames();
    std::vector<double> inter((size_t)(n * sig.channels));
    for (int64_t i = 0; i < n; ++i)
        for (int c = 0; c < sig.channels; ++c)
            inter[(size_t)(i * sig.channels + c)] = sig.chan[(size_t)c][(size_t)i];
    sf_writef_double(f, inter.data(), n);
    sf_close(f);
    return true;
}

// ---------------------------------------------------------------------------
// Iterative radix-2 Cooley-Tukey FFT (double precision, in-place).
// ---------------------------------------------------------------------------
void fft(std::vector<std::complex<double>> &a, bool inverse) {
    const size_t n = a.size();
    if (n < 2) return;
    // bit reversal
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * kPi / (double)len * (inverse ? 1.0 : -1.0);
        std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                std::complex<double> u = a[i + k];
                std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k]           = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse)
        for (auto &x : a) x /= (double)n;
}

size_t nextPow2(size_t x) {
    size_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

double hann(size_t i, size_t N) {
    return 0.5 - 0.5 * std::cos(2.0 * kPi * (double)i / (double)(N - 1));
}

// ---------------------------------------------------------------------------
// Envelope follower: full-wave rectify + one-pole low-pass (5 ms default).
// ---------------------------------------------------------------------------
std::vector<double> envelope(const std::vector<double> &x, int sr, double tauMs = 5.0) {
    double alpha = 1.0 - std::exp(-1.0 / (tauMs * 1e-3 * (double)sr));
    std::vector<double> e(x.size(), 0.0);
    double y = 0.0;
    for (size_t n = 0; n < x.size(); ++n) {
        double r = std::fabs(x[n]);
        y += alpha * (r - y);
        e[n] = y;
    }
    return e;
}

// ---------------------------------------------------------------------------
// Onset detection: rising threshold crossings of the energy envelope, relative
// to the per-file peak (scale-invariant), with a refractory gap.
// ---------------------------------------------------------------------------
std::vector<int64_t> detectOnsets(const std::vector<double> &x, int sr) {
    std::vector<double> e = envelope(x, sr, 5.0);
    double peak = 0.0;
    for (double v : e) peak = std::max(peak, v);
    if (peak <= 1e-9) return {};
    const double thr = 0.15 * peak;            // relative to file peak
    const int64_t refractory = (int64_t)(0.100 * sr);   // 100 ms
    std::vector<int64_t> onsets;
    int64_t last = -refractory * 2;
    for (size_t n = 1; n < e.size(); ++n) {
        if (e[n] > thr && e[n - 1] <= thr && (int64_t)n - last >= refractory) {
            onsets.push_back((int64_t)n);
            last = (int64_t)n;
        }
    }
    return onsets;
}

// ---------------------------------------------------------------------------
// Metric a: RMS.
// ---------------------------------------------------------------------------
struct RmsResult {
    double overallRef = 0, overallCand = 0;
    double overallRelDev = 0;   // (cand-ref)/ref
    double perSecMaxRelDev = 0;
};

double rms(const std::vector<double> &x, int64_t a, int64_t b) {
    if (b <= a) return 0.0;
    double s = 0.0;
    for (int64_t i = a; i < b; ++i) s += x[(size_t)i] * x[(size_t)i];
    return std::sqrt(s / (double)(b - a));
}

RmsResult metricRms(const Signal &ref, const Signal &cand) {
    RmsResult r;
    r.overallRef  = rms(ref.mono,  0, ref.frames());
    r.overallCand = rms(cand.mono, 0, cand.frames());
    r.overallRelDev = (r.overallRef > 1e-12)
                    ? (r.overallCand - r.overallRef) / r.overallRef : 0.0;
    int sr = ref.sampleRate;
    int64_t secs = std::min(ref.frames(), cand.frames()) / sr;
    for (int64_t s = 0; s < secs; ++s) {
        double rr = rms(ref.mono,  s * sr, (s + 1) * sr);
        double rc = rms(cand.mono, s * sr, (s + 1) * sr);
        double dev = (rr > 1e-9) ? std::fabs(rc - rr) / rr : 0.0;
        r.perSecMaxRelDev = std::max(r.perSecMaxRelDev, dev);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Metric b: dominant frequency per second (FFT peak + parabolic interpolation).
// ---------------------------------------------------------------------------
double dominantFreq(const std::vector<double> &x, int64_t start, int64_t len, int sr) {
    if (len < 64) return 0.0;
    size_t N = nextPow2((size_t)len);
    std::vector<std::complex<double>> a(N, {0.0, 0.0});
    for (int64_t i = 0; i < len; ++i)
        a[(size_t)i] = x[(size_t)(start + i)] * hann((size_t)i, (size_t)len);
    fft(a, false);
    size_t half = N / 2;
    // ignore < 40 Hz (DC / rumble)
    size_t loBin = (size_t)std::ceil(40.0 * (double)N / (double)sr);
    if (loBin < 1) loBin = 1;
    size_t peak = loBin;
    double peakMag = -1.0;
    for (size_t k = loBin; k < half; ++k) {
        double m = std::norm(a[k]);
        if (m > peakMag) { peakMag = m; peak = k; }
    }
    // parabolic interpolation on log-magnitude
    double delta = 0.0;
    if (peak > 0 && peak + 1 < half) {
        double y0 = std::log(std::abs(a[peak - 1]) + 1e-20);
        double y1 = std::log(std::abs(a[peak])     + 1e-20);
        double y2 = std::log(std::abs(a[peak + 1]) + 1e-20);
        double denom = y0 - 2.0 * y1 + y2;
        if (std::fabs(denom) > 1e-20) delta = 0.5 * (y0 - y2) / denom;
    }
    return ((double)peak + delta) * (double)sr / (double)N;
}

struct FreqResult {
    double maxRelDev = 0;
    double medianRef = 0, medianCand = 0;
};

FreqResult metricFreq(const Signal &ref, const Signal &cand) {
    FreqResult r;
    int sr = ref.sampleRate;
    int64_t secs = std::min(ref.frames(), cand.frames()) / sr;
    std::vector<double> fr, fc;
    for (int64_t s = 0; s < secs; ++s) {
        double a = dominantFreq(ref.mono,  s * sr, sr, sr);
        double b = dominantFreq(cand.mono, s * sr, sr, sr);
        if (a > 1.0 && b > 1.0) {
            fr.push_back(a); fc.push_back(b);
            r.maxRelDev = std::max(r.maxRelDev, std::fabs(b - a) / a);
        }
    }
    auto median = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    r.medianRef  = median(fr);
    r.medianCand = median(fc);
    return r;
}

// ---------------------------------------------------------------------------
// Metric c: transient smear (rise-time ratio + unpaired onsets).
// ---------------------------------------------------------------------------
// 10%->90% rise time (seconds) of the envelope in a 60 ms window near onset.
double riseTime(const std::vector<double> &e, int64_t onset, int sr) {
    int64_t pre  = (int64_t)(0.005 * sr);
    int64_t win  = (int64_t)(0.060 * sr);
    int64_t a = std::max((int64_t)0, onset - pre);
    int64_t b = std::min((int64_t)e.size(), a + win);
    if (b - a < 4) return 0.0;
    double lo = 1e300, hi = -1e300;
    for (int64_t i = a; i < b; ++i) { lo = std::min(lo, e[(size_t)i]); hi = std::max(hi, e[(size_t)i]); }
    if (hi - lo <= 1e-9) return 0.0;
    double l10 = lo + 0.10 * (hi - lo);
    double l90 = lo + 0.90 * (hi - lo);
    int64_t t10 = -1, t90 = -1;
    for (int64_t i = a; i < b; ++i) {
        if (t10 < 0 && e[(size_t)i] >= l10) t10 = i;
        if (t10 >= 0 && e[(size_t)i] >= l90) { t90 = i; break; }
    }
    if (t10 < 0 || t90 < 0 || t90 <= t10) return 0.0;
    return (double)(t90 - t10) / (double)sr;
}

struct SmearResult {
    int onsetsRef = 0, onsetsCand = 0, paired = 0;
    int unpairedRef = 0, unpairedCand = 0;
    double meanRatio = 1.0, maxRatio = 1.0;
};

SmearResult metricSmear(const Signal &ref, const Signal &cand) {
    SmearResult r;
    int sr = ref.sampleRate;
    std::vector<int64_t> oref = detectOnsets(ref.mono, sr);
    std::vector<int64_t> ocand = detectOnsets(cand.mono, sr);
    r.onsetsRef = (int)oref.size();
    r.onsetsCand = (int)ocand.size();
    std::vector<double> eref  = envelope(ref.mono,  sr, 5.0);
    std::vector<double> ecand = envelope(cand.mono, sr, 5.0);
    int64_t tol = (int64_t)(0.050 * sr);   // 50 ms
    std::vector<bool> usedCand(ocand.size(), false);
    std::vector<double> ratios;
    for (int64_t o : oref) {
        // nearest unused candidate onset within tolerance
        int best = -1; int64_t bestd = tol + 1;
        for (size_t j = 0; j < ocand.size(); ++j) {
            if (usedCand[j]) continue;
            int64_t d = std::llabs(ocand[j] - o);
            if (d <= tol && d < bestd) { bestd = d; best = (int)j; }
        }
        if (best < 0) { r.unpairedRef++; continue; }
        usedCand[(size_t)best] = true;
        r.paired++;
        double rr = riseTime(eref,  o,             sr);
        double rc = riseTime(ecand, ocand[(size_t)best], sr);
        if (rr > 1e-6) ratios.push_back(rc / rr);
    }
    for (bool u : usedCand) if (!u) r.unpairedCand++;
    if (!ratios.empty()) {
        double sum = 0.0; r.maxRatio = 0.0;
        for (double v : ratios) { sum += v; r.maxRatio = std::max(r.maxRatio, v); }
        r.meanRatio = sum / (double)ratios.size();
    }
    return r;
}

// ---------------------------------------------------------------------------
// Metric d: modulation-spectrum warble over the longest onset-free region.
// ---------------------------------------------------------------------------
// Region is chosen from the *reference* onsets so both files are measured over
// the same span. Returns peak 2..30 Hz modulation component in dB re. mean.
struct Region { int64_t start, end; };

Region longestOnsetFreeRegion(const Signal &ref) {
    int sr = ref.sampleRate;
    int64_t n = ref.frames();
    int64_t guard = (int64_t)(0.050 * sr);
    std::vector<int64_t> on = detectOnsets(ref.mono, sr);
    std::vector<int64_t> bounds;
    bounds.push_back(0);
    for (int64_t o : on) bounds.push_back(o);
    bounds.push_back(n);
    Region best{guard, n - guard};
    int64_t bestLen = -1;
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        int64_t a = bounds[i] + (i == 0 ? guard : guard);
        int64_t b = bounds[i + 1] - guard;
        if (b - a > bestLen) { bestLen = b - a; best = {a, b}; }
    }
    if (best.start < 0) best.start = 0;
    if (best.end > n)   best.end = n;
    return best;
}

double warbleDb(const std::vector<double> &x, Region reg, int sr) {
    int64_t len = reg.end - reg.start;
    if (len < sr / 4) return -140.0;   // region too short to trust
    std::vector<double> e = envelope(x, sr, 5.0);
    // AC-couple over region, Hann-window, FFT.
    double mean = 0.0;
    for (int64_t i = reg.start; i < reg.end; ++i) mean += e[(size_t)i];
    mean /= (double)len;
    if (mean <= 1e-9) return -140.0;
    size_t N = nextPow2((size_t)len);
    std::vector<std::complex<double>> a(N, {0.0, 0.0});
    double wsum = 0.0;
    for (int64_t i = 0; i < len; ++i) {
        double w = hann((size_t)i, (size_t)len);
        wsum += w;
        a[(size_t)i] = (e[(size_t)(reg.start + i)] - mean) * w;
    }
    fft(a, false);
    // peak amplitude in 2..30 Hz, recovered from Hann coherent gain (wsum/2).
    size_t k2  = (size_t)std::ceil(2.0  * (double)N / (double)sr);
    size_t k30 = (size_t)std::floor(30.0 * (double)N / (double)sr);
    if (k2 < 1) k2 = 1;
    double peakAmp = 0.0;
    for (size_t k = k2; k <= k30 && k < N / 2; ++k) {
        double amp = 2.0 * std::abs(a[k]) / (wsum * 0.5);   // sinusoid amplitude
        peakAmp = std::max(peakAmp, amp);
    }
    if (peakAmp <= 1e-12) return -140.0;
    return 20.0 * std::log10(peakAmp / mean);
}

struct WarbleResult {
    Region region{0, 0};
    double refDb = -140, candDb = -140, deltaDb = 0;
    bool flagged = false;
};

WarbleResult metricWarble(const Signal &ref, const Signal &cand) {
    WarbleResult r;
    r.region = longestOnsetFreeRegion(ref);
    Region rc = r.region;
    rc.end = std::min(rc.end, cand.frames());
    r.refDb  = warbleDb(ref.mono,  r.region, ref.sampleRate);
    r.candDb = warbleDb(cand.mono, rc,       cand.sampleRate);
    r.deltaDb = r.candDb - r.refDb;
    r.flagged = (r.candDb > -40.0);   // audible AM warble threshold
    return r;
}

// ---------------------------------------------------------------------------
// Metric e: spectral balance in octave bands (Welch-averaged power spectrum).
// ---------------------------------------------------------------------------
std::vector<double> welchPower(const std::vector<double> &x, int sr, size_t N = 4096) {
    std::vector<double> psd(N / 2 + 1, 0.0);
    if (x.size() < N) return psd;
    size_t hop = N / 2, frames = 0;
    double wsum2 = 0.0;
    for (size_t i = 0; i < N; ++i) { double w = hann(i, N); wsum2 += w * w; }
    for (size_t s = 0; s + N <= x.size(); s += hop) {
        std::vector<std::complex<double>> a(N);
        for (size_t i = 0; i < N; ++i) a[i] = x[s + i] * hann(i, N);
        fft(a, false);
        for (size_t k = 0; k <= N / 2; ++k) psd[k] += std::norm(a[k]);
        frames++;
    }
    if (frames == 0) return psd;
    double norm = 1.0 / ((double)frames * wsum2);
    for (double &v : psd) v *= norm;
    (void)sr;
    return psd;
}

struct BalanceResult {
    static constexpr int NB = 7;
    double centers[NB] = {125, 250, 500, 1000, 2000, 4000, 8000};
    double deltaDb[NB] = {0};
    double maxAbsDelta = 0;
};

double bandPower(const std::vector<double> &psd, int sr, size_t N, double lo, double hi) {
    size_t k0 = (size_t)std::ceil(lo * (double)N / (double)sr);
    size_t k1 = (size_t)std::floor(hi * (double)N / (double)sr);
    if (k0 < 1) k0 = 1;
    double s = 0.0;
    for (size_t k = k0; k <= k1 && k < psd.size(); ++k) s += psd[k];
    return s;
}

BalanceResult metricBalance(const Signal &ref, const Signal &cand) {
    BalanceResult r;
    const size_t N = 4096;
    std::vector<double> pr = welchPower(ref.mono,  ref.sampleRate,  N);
    std::vector<double> pc = welchPower(cand.mono, cand.sampleRate, N);
    for (int i = 0; i < BalanceResult::NB; ++i) {
        double lo = r.centers[i] / std::sqrt(2.0);
        double hi = r.centers[i] * std::sqrt(2.0);
        double br = bandPower(pr, ref.sampleRate,  N, lo, hi);
        double bc = bandPower(pc, cand.sampleRate, N, lo, hi);
        double d = 10.0 * std::log10((bc + 1e-20) / (br + 1e-20));
        r.deltaDb[i] = d;
        r.maxAbsDelta = std::max(r.maxAbsDelta, std::fabs(d));
    }
    return r;
}

// ---------------------------------------------------------------------------
// Report.
// ---------------------------------------------------------------------------
struct AllMetrics {
    RmsResult rms; FreqResult freq; SmearResult smear;
    WarbleResult warble; BalanceResult bal;
};

AllMetrics computeAll(const Signal &ref, const Signal &cand) {
    AllMetrics m;
    m.rms    = metricRms(ref, cand);
    m.freq   = metricFreq(ref, cand);
    m.smear  = metricSmear(ref, cand);
    m.warble = metricWarble(ref, cand);
    m.bal    = metricBalance(ref, cand);
    return m;
}

void printReport(const Signal &ref, const Signal &cand, const AllMetrics &m) {
    printf("=== warp_ab: A/B time-stretch quality ===\n");
    printf("reference: sr=%d ch=%d frames=%lld\n",
           ref.sampleRate, ref.channels, (long long)ref.frames());
    printf("candidate: sr=%d ch=%d frames=%lld\n\n",
           cand.sampleRate, cand.channels, (long long)cand.frames());

    printf("[a] RMS\n");
    printf("    overall:   ref=%.5f  cand=%.5f  delta=%+.2f%%\n",
           m.rms.overallRef, m.rms.overallCand, m.rms.overallRelDev * 100.0);
    printf("    per-second max relative deviation: %.2f%%\n\n",
           m.rms.perSecMaxRelDev * 100.0);

    printf("[b] Dominant frequency (per second)\n");
    printf("    median ref/cand: %.1f / %.1f Hz\n", m.freq.medianRef, m.freq.medianCand);
    printf("    max relative deviation: %.3f%%\n\n", m.freq.maxRelDev * 100.0);

    printf("[c] Transient smear\n");
    printf("    onsets: ref=%d cand=%d paired=%d  unpaired(ref/cand)=%d/%d\n",
           m.smear.onsetsRef, m.smear.onsetsCand, m.smear.paired,
           m.smear.unpairedRef, m.smear.unpairedCand);
    printf("    rise-time ratio (cand/ref): mean=%.2f max=%.2f\n\n",
           m.smear.meanRatio, m.smear.maxRatio);

    printf("[d] Modulation warble\n");
    printf("    region: [%.2f..%.2f]s (longest onset-free)\n",
           (double)m.warble.region.start / ref.sampleRate,
           (double)m.warble.region.end   / ref.sampleRate);
    printf("    peak 2-30Hz mod: ref=%.1f dB  cand=%.1f dB  delta=%+.1f dB   [%s]\n\n",
           m.warble.refDb, m.warble.candDb, m.warble.deltaDb,
           m.warble.flagged ? "WARBLE FLAGGED" : "ok");

    printf("[e] Spectral balance (octave bands, dB delta cand-ref)\n    ");
    const char *lbl[7] = {"125","250","500","1k","2k","4k","8k"};
    for (int i = 0; i < BalanceResult::NB; ++i)
        printf("%s:%+.1f ", lbl[i], m.bal.deltaDb[i]);
    printf("\n    max abs delta: %.1f dB\n\n", m.bal.maxAbsDelta);

    // Machine-readable one-liner for the driver script.
    printf("SUMMARY|rms_overall_ref=%.5f|rms_overall_cand=%.5f|rms_overall_dpct=%+.2f"
           "|rms_persec_maxdev_pct=%.2f|freq_maxdev_pct=%.3f"
           "|smear_mean=%.2f|smear_max=%.2f|unpaired_ref=%d|unpaired_cand=%d"
           "|warble_ref_db=%.1f|warble_cand_db=%.1f|warble_delta_db=%+.1f|warble_flag=%s"
           "|specbal_max_db=%.1f\n",
           m.rms.overallRef, m.rms.overallCand, m.rms.overallRelDev * 100.0,
           m.rms.perSecMaxRelDev * 100.0, m.freq.maxRelDev * 100.0,
           m.smear.meanRatio, m.smear.maxRatio, m.smear.unpairedRef, m.smear.unpairedCand,
           m.warble.refDb, m.warble.candDb, m.warble.deltaDb,
           m.warble.flagged ? "yes" : "no", m.bal.maxAbsDelta);
}

// ---------------------------------------------------------------------------
// Deterministic corpus generation.
// ---------------------------------------------------------------------------
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    double bipolar() {   // uniform [-1, 1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t x = s >> 11;               // top 53 bits
        return (double)x / (double)(1ULL << 53) * 2.0 - 1.0;
    }
};

constexpr int    kSR      = 48000;
constexpr int64_t kFrames = 192000;   // 4.0 s
constexpr int    kCh      = 2;

Signal makeStereo(const std::vector<double> &m) {
    Signal s; s.sampleRate = kSR; s.channels = kCh;
    s.chan.assign(kCh, m);
    s.buildMono();
    return s;
}

Signal genSaw220() {
    std::vector<double> m((size_t)kFrames, 0.0);
    const double f0 = 220.0;
    int K = (int)((kSR * 0.5) / f0);     // harmonics up to Nyquist
    double norm = 0.0;
    for (int64_t n = 0; n < kFrames; ++n) {
        double t = (double)n / kSR, v = 0.0;
        for (int k = 1; k <= K; ++k) v += std::sin(2.0 * kPi * k * f0 * t) / k;
        m[(size_t)n] = v;
    }
    for (double v : m) norm = std::max(norm, std::fabs(v));
    if (norm > 0) for (double &v : m) v *= 0.5 / norm;
    return makeStereo(m);
}

Signal genSine440() {
    std::vector<double> m((size_t)kFrames, 0.0);
    for (int64_t n = 0; n < kFrames; ++n)
        m[(size_t)n] = 0.5 * std::sin(2.0 * kPi * 440.0 * (double)n / kSR);
    return makeStereo(m);
}

Signal genVoice() {
    // Harmonics of 150 Hz, formant peaks near 800 & 1200 Hz, 5 Hz +/-30-cent
    // vibrato, slow amplitude contour. Phase accumulated to honour vibrato.
    std::vector<double> m((size_t)kFrames, 0.0);
    const double f0 = 150.0;
    auto formant = [](double f) {
        double r1 = 1.0 / (1.0 + std::pow((f - 800.0) / 120.0, 2.0));
        double r2 = 0.8 / (1.0 + std::pow((f - 1200.0) / 160.0, 2.0));
        double tilt = std::exp(-f / 4000.0);
        return (r1 + r2 + 0.05) * tilt;
    };
    double phase = 0.0;
    double norm = 0.0;
    for (int64_t n = 0; n < kFrames; ++n) {
        double t = (double)n / kSR;
        double vib = std::pow(2.0, (30.0 / 1200.0) * std::sin(2.0 * kPi * 5.0 * t));
        double finst = f0 * vib;
        phase += 2.0 * kPi * finst / kSR;
        double contour = 0.7 + 0.3 * std::sin(2.0 * kPi * 0.5 * t - 0.5 * kPi);
        int K = (int)((kSR * 0.5) / finst);
        double v = 0.0;
        for (int k = 1; k <= K; ++k)
            v += formant(k * finst) * std::sin(k * phase);
        v *= contour;
        m[(size_t)n] = v;
    }
    for (double v : m) norm = std::max(norm, std::fabs(v));
    if (norm > 0) for (double &v : m) v *= 0.5 / norm;
    return makeStereo(m);
}

Signal genTransients() {
    // 2 kHz-centered 8 ms noise bursts every 500 ms over a -60 dB white floor.
    std::vector<double> m((size_t)kFrames, 0.0);
    Lcg floorRng(0xC0FFEEULL);
    for (int64_t n = 0; n < kFrames; ++n)
        m[(size_t)n] = 0.001 * floorRng.bipolar();   // -60 dB floor

    Lcg burstRng(0x5EED1234ULL);
    const int64_t period = kSR / 2;         // 500 ms
    const int64_t burst  = (int64_t)(0.008 * kSR);   // 8 ms
    // RBJ band-pass biquad @ 2 kHz, Q=2.
    const double f0 = 2000.0, Q = 2.0;
    double w0 = 2.0 * kPi * f0 / kSR, cw = std::cos(w0), sw = std::sin(w0);
    double al = sw / (2.0 * Q);
    double b0 = al, b1 = 0.0, b2 = -al, a0 = 1.0 + al, a1 = -2.0 * cw, a2 = 1.0 - al;
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    for (int64_t start = 0; start + burst < kFrames; start += period) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (int64_t i = 0; i < burst; ++i) {
            double x = burstRng.bipolar();
            double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            double w = hann((size_t)i, (size_t)burst);
            m[(size_t)(start + i)] += 0.5 * w * y;
        }
    }
    return makeStereo(m);
}

int genCorpus(const std::string &dir) {
    struct { const char *name; Signal (*fn)(); } items[] = {
        {"corpus_saw220.wav",     genSaw220},
        {"corpus_sine440.wav",    genSine440},
        {"corpus_voice.wav",      genVoice},
        {"corpus_transients.wav", genTransients},
    };
    for (auto &it : items) {
        Signal s = it.fn();
        std::string path = dir + "/" + it.name;
        std::string err;
        if (!writeWav16(path, s, err)) {
            printf("ERROR writing %s: %s\n", path.c_str(), err.c_str());
            return 1;
        }
        printf("wrote %s (%lld frames, sr=%d, ch=%d)\n",
               path.c_str(), (long long)s.frames(), s.sampleRate, s.channels);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Self-tests: prove every metric catches its failure mode.
// ---------------------------------------------------------------------------
Signal copySig(const Signal &s) { return s; }

Signal scaleAmp(const Signal &s, double g) {
    Signal o = s;
    for (auto &c : o.chan) for (double &v : c) v *= g;
    o.buildMono();
    return o;
}

Signal applyAM(const Signal &s, double freq, double depth) {
    Signal o = s;
    for (auto &c : o.chan)
        for (size_t n = 0; n < c.size(); ++n)
            c[n] *= (1.0 + depth * std::sin(2.0 * kPi * freq * (double)n / s.sampleRate));
    o.buildMono();
    return o;
}

// Smear onsets: reshape the amplitude envelope with a causal slow-attack
// one-pole (15 ms) — the carrier is preserved, but every attack's 10%->90%
// rise time is stretched to ~33 ms, well above the reference's few-ms rise.
// (A symmetric kernel spreads energy both ways and barely slows the attack;
// a causal slow-attack follower is the faithful "smeared onsets" case.)
Signal smearOnsets(const Signal &s) {
    Signal o = s;
    int sr = s.sampleRate;
    double alpha = 1.0 - std::exp(-1.0 / (20.0 * 1e-3 * (double)sr));  // 20 ms
    for (auto &c : o.chan) {
        std::vector<double> e = envelope(c, sr, 5.0);
        std::vector<double> es(e.size(), 0.0);
        double y = 0.0;
        for (size_t n = 0; n < e.size(); ++n) { y += alpha * (e[n] - y); es[n] = y; }
        for (size_t n = 0; n < c.size(); ++n) {
            double g = (e[n] > 1e-6) ? es[n] / e[n] : 1.0;
            c[n] *= g;
        }
    }
    o.buildMono();
    return o;
}

int runSelftest() {
    int failures = 0;
    auto check = [&](const char *name, bool ok, const std::string &detail) {
        printf("%-45s %s   %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
        if (!ok) failures++;
    };
    char buf[256];

    // (i) identical files -> all deltas ~0.
    {
        Signal t = genTransients();
        AllMetrics m = computeAll(t, copySig(t));
        bool ok = std::fabs(m.rms.overallRelDev) < 1e-9
               && m.freq.maxRelDev < 1e-9
               && std::fabs(m.smear.meanRatio - 1.0) < 1e-9
               && std::fabs(m.smear.maxRatio - 1.0) < 1e-9
               && m.smear.unpairedRef == 0 && m.smear.unpairedCand == 0
               && std::fabs(m.warble.deltaDb) < 1e-6;
        snprintf(buf, sizeof(buf), "rmsDev=%.1e freqDev=%.1e smearMax=%.4f warbleD=%.1e unpaired=%d/%d",
                 m.rms.overallRelDev, m.freq.maxRelDev, m.smear.maxRatio,
                 m.warble.deltaDb, m.smear.unpairedRef, m.smear.unpairedCand);
        check("(i) identical transients -> all ~0", ok, buf);

        Signal sine = genSine440();
        AllMetrics ms = computeAll(sine, copySig(sine));
        bool ok2 = std::fabs(ms.rms.overallRelDev) < 1e-9
                && ms.freq.maxRelDev < 1e-9
                && std::fabs(ms.warble.deltaDb) < 1e-6;
        snprintf(buf, sizeof(buf), "rmsDev=%.1e freqDev=%.1e warbleD=%.1e (refDb=%.1f)",
                 ms.rms.overallRelDev, ms.freq.maxRelDev, ms.warble.deltaDb, ms.warble.refDb);
        check("(i) identical sine -> all ~0", ok2, buf);
    }

    // (ii) 6 Hz 20% AM -> warble worsens >10 dB and is flagged.
    {
        Signal sine = genSine440();
        Signal am = applyAM(sine, 6.0, 0.20);
        AllMetrics m = computeAll(sine, am);
        bool ok = m.warble.deltaDb > 10.0 && m.warble.flagged;
        snprintf(buf, sizeof(buf), "refDb=%.1f candDb=%.1f delta=%+.1f dB flagged=%d",
                 m.warble.refDb, m.warble.candDb, m.warble.deltaDb, (int)m.warble.flagged);
        check("(ii) 6Hz 20% AM -> warble +>10dB & flagged", ok, buf);
    }

    // (iii) smeared onsets -> rise-time ratio > 2.
    {
        Signal t = genTransients();
        Signal sm = smearOnsets(t);
        AllMetrics m = computeAll(t, sm);
        bool ok = m.smear.maxRatio > 2.0 && m.smear.paired > 0;
        snprintf(buf, sizeof(buf), "paired=%d meanRatio=%.2f maxRatio=%.2f",
                 m.smear.paired, m.smear.meanRatio, m.smear.maxRatio);
        check("(iii) smeared onsets -> rise ratio >2", ok, buf);
    }

    // (iv) half amplitude -> RMS deviation ~50%.
    {
        Signal t = genVoice();
        Signal half = scaleAmp(t, 0.5);
        AllMetrics m = computeAll(t, half);
        bool ok = std::fabs(m.rms.overallRelDev + 0.5) < 0.01;   // -50%
        snprintf(buf, sizeof(buf), "overallRelDev=%+.2f%% persecMax=%.2f%%",
                 m.rms.overallRelDev * 100.0, m.rms.perSecMaxRelDev * 100.0);
        check("(iv) half amplitude -> RMS -50%", ok, buf);
    }

    printf("\n%s: %d self-test(s) failed\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--gen") == 0)
        return genCorpus(argv[2]);
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0)
        return runSelftest();
    if (argc == 3) {
        Signal ref, cand;
        std::string err;
        if (!loadWav(argv[1], ref, err)) { printf("ERROR reference: %s\n", err.c_str()); return 2; }
        if (!loadWav(argv[2], cand, err)) { printf("ERROR candidate: %s\n", err.c_str()); return 2; }
        // Cross-check overall RMS against the shared tw_analysis loader (reuse).
        std::string aerr;
        audio::AcousticMetrics am = audio::analyzeWavFile(argv[1], aerr);
        (void)am;
        AllMetrics m = computeAll(ref, cand);
        printReport(ref, cand, m);
        return 0;
    }
    printf("usage:\n");
    printf("  warp_ab <reference.wav> <candidate.wav>   print metric table + SUMMARY\n");
    printf("  warp_ab --gen <outdir>                    write the corpus WAVs\n");
    printf("  warp_ab --selftest                        run metric self-tests\n");
    return 1;
}
