// perf_core.hpp — the platform-free half of the perf log (stage 6, perflog.cpp is the 3DS half): running statistics
// of a second, a session-wide frame-time histogram and the CSV row builder that keeps the header and the values in step.
// Builds on the PC for tests/perf_test.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace perf {

// count / sum / min / max of a value over a window
struct Acc {
    double sum = 0, mn = 0, mx = 0;
    uint32_t n = 0;
    void add(double v) {
        if (!n) { mn = mx = v; } else { mn = std::min(mn, v); mx = std::max(mx, v); }
        sum += v;
        ++n;
    }
    double avg() const { return n ? sum / n : 0.0; }
    double max() const { return n ? mx : 0.0; }
    double min() const { return n ? mn : 0.0; }
    void reset() { *this = Acc(); }
};

// p in 0..1 of the values (sorted in place); 0 when empty
inline double percentile(std::vector<float>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, (size_t)std::floor(p * (double)(v.size() - 1) + 0.5));
    return v[i];
}

// Frame times of the whole session in 0.5 ms bins up to 250 ms (the last bin takes everything longer).
struct Histogram {
    static constexpr int kBins = 501;
    static constexpr double kBin = 0.5;
    uint32_t bins[kBins] = {};
    uint64_t n = 0;
    double sum = 0, mx = 0;
    void add(double ms) {
        int b = (int)(ms / kBin);
        bins[std::clamp(b, 0, kBins - 1)]++;
        ++n;
        sum += ms;
        mx = std::max(mx, ms);
    }
    // the upper edge of the bin holding the p-th value
    double percentile(double p) const {
        if (!n) return 0.0;
        const uint64_t want = (uint64_t)std::ceil(p * (double)n);
        uint64_t seen = 0;
        for (int b = 0; b < kBins; ++b) {
            seen += bins[b];
            if (seen >= std::max<uint64_t>(want, 1)) return b == kBins - 1 ? mx : (b + 1) * kBin;
        }
        return mx;
    }
    uint64_t over(double ms) const {   // frames longer than ms (bin resolution)
        uint64_t c = 0;
        for (int b = (int)(ms / kBin); b < kBins; ++b) c += bins[b];
        return c;
    }
};

// One CSV line. The first line's column names become the header; a later line with other names (a new build's
// columns, a mistake) reports headerChanged() so the writer can start a new header. The values are kept as numbers and
// turned into text by format() - on the 3DS in the perf log's writer thread (stage 6: printing ~350 floats with newlib
// is a millisecond or more of the main thread).
class Row {
public:
    struct Val {
        double d = 0;
        long long i = 0;
        int prec = 0;
        uint8_t kind = 0;   // 0 number, 1 integer, 2 text
        std::string s;
    };
    void begin() {
        prev_.swap(names_);
        names_.clear();
        vals_.clear();
    }
    void num(const char* name, double v, int prec = 2) {
        Val x;
        x.d = std::isfinite(v) ? v : 0.0;
        x.prec = prec;
        put(name, std::move(x));
    }
    void integer(const char* name, long long v) {
        Val x;
        x.i = v;
        x.kind = 1;
        put(name, std::move(x));
    }
    void text(const char* name, const char* v) {   // no commas, quotes or line breaks in a field
        Val x;
        x.kind = 2;
        x.s = v ? v : "";
        for (char& c : x.s)
            if (c == ',' || c == '"' || c == '\n' || c == '\r') c = ';';
        put(name, std::move(x));
    }
    static void formatVal(const Val& x, std::string& out) {
        char b[48];
        if (x.kind == 2) { out += x.s; return; }
        if (x.kind == 1) { snprintf(b, sizeof b, "%lld", x.i); out += b; return; }
        int n = snprintf(b, sizeof b, "%.*f", x.prec, x.d);
        if (x.prec > 0 && n > 0) {   // 12.50 -> 12.5, 3.00 -> 3
            while (n > 1 && b[n - 1] == '0') --n;
            if (n > 1 && b[n - 1] == '.') --n;
            b[n] = 0;
        }
        if (!strcmp(b, "-0")) strcpy(b, "0");
        out += b;
    }
    static void format(const std::vector<Val>& vals, std::string& out) {
        for (size_t k = 0; k < vals.size(); ++k) {
            if (k) out += ',';
            formatVal(vals[k], out);
        }
        out += '\n';
    }
    std::string header() const {
        std::string h;
        for (size_t i = 0; i < names_.size(); ++i) { if (i) h += ','; h += names_[i]; }
        h += '\n';
        return h;
    }
    std::string line() const { std::string o; format(vals_, o); return o; }
    std::vector<Val> takeValues() { std::vector<Val> v; v.swap(vals_); return v; }
    size_t columns() const { return names_.size(); }
    bool headerChanged() const {
        if (prev_.size() != names_.size()) return true;
        for (size_t i = 0; i < names_.size(); ++i)
            if (prev_[i] != names_[i]) return true;
        return false;
    }

private:
    void put(const char* name, Val&& v) {
        vals_.push_back(std::move(v));
        names_.push_back(name);
    }
    std::vector<std::string> names_, prev_;
    std::vector<Val> vals_;
};

// A level that is reported once when it falls under a threshold and again only after it rose `hysteresis` above it.
struct LowWatch {
    double threshold = 0, hysteresis = 0;
    bool armed = true;
    bool check(double v) {   // true: just went under
        if (armed && v < threshold) { armed = false; return true; }
        if (!armed && v > threshold + hysteresis) armed = true;
        return false;
    }
};

}  // namespace perf
