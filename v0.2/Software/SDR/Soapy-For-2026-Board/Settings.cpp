// Settings.cpp — Constructor, Si5351a math, frequency/sample-rate control.
//
// The Si5351a computation is a direct C++ port of the Python in
// quisk_conf_2026.py.  The CDC serial protocol is identical to that file.
//
// Protocol summary (115200 8N1, all lines \n-terminated):
//   → VER          ← VER,<version_string>
//   → XTAL         ← XTAL,<hz_float>
//   → MODE         ← MODE,DIRECT  or  MODE,JOHNSON
//   → RATE,<N>     ← OK
//   → FREQ,<si5351_hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>
//                  ← (one blank/echo line)
//                  ← OK,G,<signed_offset>   (G = integer/golden)
//                  or OK,F,<signed_offset>   (F = fractional)
//   → MODE,DIRECT  ← OK
//   → MODE,JOHNSON ← OK

#include "Soapy2026SDR.hpp"
#include <SoapySDR/Logger.hpp>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <sstream>

// ─── Constructor ─────────────────────────────────────────────────────────────

Soapy2026SDR::Soapy2026SDR(const SoapySDR::Kwargs &args)
    : _johnsonMode(false)
    , _johnsonAvailable(false)
    , _crystalFreq(24576000.0)
    , _centerFreq(7100000.0)
    , _audio(nullptr)
    , _sampleRate(48000.0)
    , _audioDeviceName("WWU SDR")
    , _ringHead(0), _ringTail(0), _ringCount(0), _ringOverflow(false)
{
    // Audio device name can be overridden via args
    if (args.count("audio_label")) {
        _audioDeviceName = args.at("audio_label");
        _audioLabelExplicit = true;
    }

    // ── Open serial port ──────────────────────────────────────────────────
    _serialPath = args.count("serial_port") ? args.at("serial_port") : "";
    if (_serialPath.empty())
        throw std::runtime_error("Soapy2026SDR: no serial_port specified");

    if (!_serial.open(_serialPath, 115200))
        throw std::runtime_error("Soapy2026SDR: cannot open " + _serialPath);

    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: opened serial %s", _serialPath.c_str());

    // ── Board reset + ready handshake (mirrors quisk_conf_2026.py) ────────
    char ctrlcd[] = {0x03, 0x04, 0};
    _serial.writeLine(std::string(ctrlcd));
    _serial.flushInput();

    // Wait for "SDR ready"
    for (int i = 0; i < 30; i++) {
        std::string line = _serialReadLine(200);
        if (line.find("SDR ready") != std::string::npos) break;
    }

    // ── Query firmware parameters ─────────────────────────────────────────
    _firmwareVersion = _getParam("VER");
    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: firmware %s", _firmwareVersion.c_str());

    std::string xtalStr = _getParam("XTAL");
    double xtal = std::atof(xtalStr.c_str());
    if (xtal > 0.0) _crystalFreq = xtal;
    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: crystal %.3f Hz", _crystalFreq);

    std::string modeStr = _getParam("MODE");
    // Firmware returns "JOHNSON" only on v0.2; anything else means DIRECT
    // (older firmware may reply "OK" or not implement MODE at all)
    _johnsonMode      = (modeStr == "JOHNSON");
    _johnsonAvailable = (modeStr == "JOHNSON");
    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: mode %s",
                  _johnsonMode ? "JOHNSON" : "DIRECT");

    // ── Set initial sample rate ───────────────────────────────────────────
    _setParam("RATE", std::to_string((int)_sampleRate));

    // ── Pre-allocate ring buffer ──────────────────────────────────────────
    _ring.assign(RING_FRAMES * 2, 0.0f); // *2 for I+Q per frame

    // ── Initialize miniaudio (sets up _audio) ────────────────────────────
    _setupAudio(); // defined in Streaming.cpp
}

Soapy2026SDR::~Soapy2026SDR()
{
    _teardownAudio(); // defined in Streaming.cpp
    _serial.close();
}

// ─── Identification ───────────────────────────────────────────────────────────

std::string Soapy2026SDR::getDriverKey() const   { return "2026SDR"; }

std::string Soapy2026SDR::getHardwareKey() const
{
    return _johnsonAvailable ? "WWU-2026-v0.2" : "WWU-2026-v0.1";
}

SoapySDR::Kwargs Soapy2026SDR::getHardwareInfo() const
{
    SoapySDR::Kwargs info;
    info["firmware"]    = _firmwareVersion;
    info["crystal_hz"]  = std::to_string(_crystalFreq);
    info["mixer_mode"]  = _johnsonMode ? "JOHNSON" : "DIRECT";
    info["johnson_ok"]  = _johnsonAvailable ? "yes" : "no";
    info["serial_port"] = _serialPath;
    info["audio_label"] = _audioDeviceName;
    return info;
}

// ─── Channels ────────────────────────────────────────────────────────────────

size_t Soapy2026SDR::getNumChannels(int direction) const
{
    return (direction == SOAPY_SDR_RX) ? 1 : 0;
}

// ─── Antenna ─────────────────────────────────────────────────────────────────

std::vector<std::string> Soapy2026SDR::listAntennas(int, size_t) const
{ return {"RX"}; }

void Soapy2026SDR::setAntenna(int, size_t, const std::string &) {}

std::string Soapy2026SDR::getAntenna(int, size_t) const { return "RX"; }

// ─── Gain (PCM1808 has no software-adjustable gain) ──────────────────────────

std::vector<std::string> Soapy2026SDR::listGains(int, size_t) const { return {}; }

// ─── Sample Rate ─────────────────────────────────────────────────────────────

std::vector<double> Soapy2026SDR::listSampleRates(int, size_t) const
{ return {48000.0, 96000.0}; }

double Soapy2026SDR::getSampleRate(int, size_t) const { return _sampleRate; }

void Soapy2026SDR::setSampleRate(int, size_t, double rate)
{
    if (rate != 48000.0 && rate != 96000.0)
        throw std::runtime_error("2026SDR: unsupported sample rate (use 48000 or 96000)");
    if (rate == _sampleRate) return;
    _sampleRate = rate;
    _setParam("RATE", std::to_string((int)rate));
    // Restart audio device at new rate
    _teardownAudio();
    _setupAudio();
    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: sample rate set to %.0f Hz", rate);
}

// ─── Frequency ───────────────────────────────────────────────────────────────

std::vector<std::string> Soapy2026SDR::listFrequencies(int, size_t) const
{ return {"RF"}; }

SoapySDR::RangeList Soapy2026SDR::getFrequencyRange(int, size_t,
                                                      const std::string &) const
{
    // v0.1 DIRECT only: 3.8 – 30 MHz
    // v0.2 DIRECT+JOHNSON: 500 kHz – 30 MHz
    double lo = _johnsonAvailable ? 500e3 : 3.8e6;
    return {SoapySDR::Range(lo, 30e6)};
}

double Soapy2026SDR::getFrequency(int, size_t, const std::string &) const
{ return _centerFreq; }

void Soapy2026SDR::setFrequency(int dir, size_t ch, const std::string &name,
                                 double freq, const SoapySDR::Kwargs &)
{
    if (dir != SOAPY_SDR_RX || ch != 0 || name != "RF") return;

    // Clamp to supported range
    double lo = _johnsonAvailable ? 500e3 : 3.8e6;
    freq = std::max(lo, std::min(30e6, freq));

    // Auto-switch DIRECT ↔ JOHNSON on v0.2 boards
    if (_johnsonAvailable) {
        bool needJohnson = (freq < 3.8e6);
        if (needJohnson != _johnsonMode) {
            _johnsonMode = needJohnson;
            _setParam("MODE", _johnsonMode ? "JOHNSON" : "DIRECT");
            SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: switched to %s mode",
                          _johnsonMode ? "JOHNSON" : "DIRECT");
        }
    }

    _programSi5351(freq);
    _centerFreq = freq;
}

// ─── Settings / sensors ──────────────────────────────────────────────────────

SoapySDR::ArgInfoList Soapy2026SDR::getSettingInfo() const
{
    SoapySDR::ArgInfoList infos;
    auto make = [](const std::string &k, const std::string &desc) {
        SoapySDR::ArgInfo i;
        i.key = k; i.name = k; i.description = desc;
        i.type = SoapySDR::ArgInfo::STRING;
        return i;
    };
    infos.push_back(make("firmware_version", "Firmware version string"));
    infos.push_back(make("crystal_freq_hz",  "Si5351a reference crystal frequency (Hz)"));
    infos.push_back(make("mixer_mode",        "Current mixer mode: DIRECT or JOHNSON"));
    infos.push_back(make("pll_status",        "Last PLL programming status from firmware"));
    return infos;
}

std::string Soapy2026SDR::readSetting(const std::string &key) const
{
    if (key == "firmware_version") return _firmwareVersion;
    if (key == "crystal_freq_hz")  return std::to_string(_crystalFreq);
    if (key == "mixer_mode")       return _johnsonMode ? "JOHNSON" : "DIRECT";
    if (key == "pll_status")       return _pllStatus;
    return "";
}

// ─── Serial helpers ───────────────────────────────────────────────────────────

void Soapy2026SDR::_serialWrite(const std::string &line)
{
    _serial.writeLine(line);
}

std::string Soapy2026SDR::_serialReadLine(int timeoutMs)
{
    return _serial.readLine(timeoutMs);
}

std::string Soapy2026SDR::_getParam(const std::string &cmd)
{
    _serialWrite(cmd);
    // Read lines until we find "CMD,value" or a terminal "OK"/"ERR"
    for (int attempt = 0; attempt < 10; attempt++) {
        std::string line = _serialReadLine(500);
        if (line.empty()) continue;
        if (line == "OK" || line == "ERR") return line;
        auto comma = line.find(',');
        if (comma != std::string::npos)
            return line.substr(comma + 1);
    }
    return "";
}

void Soapy2026SDR::_setParam(const std::string &cmd, const std::string &val)
{
    _serialWrite(cmd + "," + val);
    // Read until OK or ERR
    for (int attempt = 0; attempt < 10; attempt++) {
        std::string line = _serialReadLine(500);
        if (line.empty()) continue;
        if (line == "OK" || line == "ERR") return;
    }
}

// ─── Si5351a math ─────────────────────────────────────────────────────────────
//
// Port of quisk_conf_2026.py: _candidate_for_lo() + _candidate_from_values()
//
// The Si5351a VCO must run between 600 MHz and 900 MHz.
//   VCO = crystal × M   where M = a + b/c, 14 < M < 91
//   CLK = VCO / N
//   In DIRECT mode: CLK = rf_hz,       N must be even, 6 ≤ N ≤ 126
//   In JOHNSON mode: CLK = 4 × rf_hz,  N odd allowed,  4 ≤ N ≤ 127
//
// Register values:
//   floor_term = floor(128 × b / c)
//   P1 = 128×a + floor_term − 512
//   P2 = 128×b − c × floor_term
//   P3 = c
//
// Command: FREQ,<si5351_hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>

// Compute rational approximation num/den with denominator ≤ maxDen.
// Implements Python's Fraction.limit_denominator() exactly.
std::pair<int64_t,int64_t>
Soapy2026SDR::_limitDenominator(int64_t num, int64_t den, int64_t maxDen)
{
    // GCD reduction
    auto gcd = [](int64_t a, int64_t b) -> int64_t {
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        while (b) { a %= b; std::swap(a, b); } return a;
    };
    int64_t g = gcd(num, den);
    if (g > 0) { num /= g; den /= g; }
    if (den <= maxDen) return {num, den};

    int64_t origNum = num, origDen = den;
    int64_t p0=0, q0=1, p1=1, q1=0;
    int64_t n = num, d = den;
    while (true) {
        int64_t a  = n / d;
        int64_t q2 = q0 + a * q1;
        if (q2 > maxDen) break;
        int64_t p2 = p0 + a * p1;
        p0 = p1; q0 = q1;
        p1 = p2; q1 = q2;
        int64_t newN = d; d = n - a * d; n = newN;
    }
    int64_t k  = (maxDen - q0) / q1;
    int64_t pa = p0 + k * p1, qa = q0 + k * q1;
    int64_t pb = p1,          qb = q1;

    // Pick the closer of the two candidates to origNum/origDen
    // |pa/qa - orig| vs |pb/qb - orig|
    // Multiply through by qa*qb*origDen to avoid floating point
    // diff_a = |pa*origDen - origNum*qa| * qb
    // diff_b = |pb*origDen - origNum*qb| * qa
    double diff_a = std::abs((double)pa * origDen - (double)origNum * qa) * qb;
    double diff_b = std::abs((double)pb * origDen - (double)origNum * qb) * qa;
    return (diff_b <= diff_a) ? std::make_pair(pb, qb)
                              : std::make_pair(pa, qa);
}

Si5351Params Soapy2026SDR::_computeSi5351(double rfHz) const
{
    double si5351d = rfHz * (_johnsonMode ? 4.0 : 1.0);
    int64_t si5351_hz = (int64_t)std::round(si5351d);

    int nStep    = _johnsonMode ? 1 : 2;
    int nMinBase = _johnsonMode ? 4 : 6;
    int nMaxBase = _johnsonMode ? 127 : 126;

    int nMin = std::max(nMinBase, (int)std::ceil(600000000.0 / si5351d));
    int nMax = std::min(nMaxBase, (int)(900000000.0 / si5351d));
    if (!_johnsonMode && nMin % 2 != 0) nMin++;

    if (nMin > nMax) {
        // Frequency out of Si5351 range — return zeros (will fail gracefully)
        return {si5351_hz, nMin, 0, 0, 1, 0, 0, 1};
    }

    // Try to find an integer multiplier M (b=0) first
    int     bestN = -1;
    int64_t exactM = 0;
    for (int n = nMin; n <= nMax; n += nStep) {
        double mf = si5351d * n / _crystalFreq;
        if (mf <= 14.0 || mf >= 91.0) continue;
        int64_t mi = (int64_t)std::round(mf);
        if (std::abs(mf - (double)mi) < 1e-4) {
            bestN = n; exactM = mi; break;
        }
    }

    int64_t a, b, c;
    int     nVal;
    if (bestN >= 0) {
        nVal = bestN; a = exactM; b = 0; c = 1;
    } else {
        // Fractional: best rational approx to (si5351_hz × nMin) / crystal
        nVal = nMin;
        int64_t num = si5351_hz * (int64_t)nVal;
        int64_t den = (int64_t)std::round(_crystalFreq);
        std::pair<int64_t,int64_t> frac = _limitDenominator(num, den, 1048575LL);
        a = frac.first / frac.second;
        b = frac.first - a * frac.second;
        c = frac.second;
    }

    // Register values
    int64_t floorTerm = (int64_t)std::floor(128.0 * b / c);
    int64_t p1 = 128 * a + floorTerm - 512;
    int64_t p2 = 128 * b - c * floorTerm;
    int64_t p3 = c;

    return {si5351_hz, nVal, a, b, c, p1, p2, p3};
}

void Soapy2026SDR::_programSi5351(double rfHz)
{
    Si5351Params p = _computeSi5351(rfHz);

    std::ostringstream cmd;
    cmd << "FREQ,"
        << p.si5351_hz << ","
        << p.n << ","
        << p.a << ","
        << p.b << ","
        << p.c << ","
        << p.p1 << ","
        << p.p2 << ","
        << p.p3;

    _serialWrite(cmd.str());

    // Read firmware response:
    //   - Some firmwares send one echo/blank line then "OK,G/F,<offset>"
    //   - Simpler firmwares just send "OK"
    // Read up to 3 lines looking for the OK.
    std::string ok;
    for (int attempt = 0; attempt < 3; attempt++) {
        std::string line = _serialReadLine(300);
        if (line.empty()) continue;
        ok = line;
        if (ok.rfind("OK", 0) == 0) break;  // found OK line
    }

    // Parse: "OK,G,<offset>" (integer) or "OK,F,<offset>" (fractional) or plain "OK"
    _pllStatus = ok;
    if (ok.rfind("OK", 0) == 0) {
        // Tokenize by comma
        std::vector<std::string> parts;
        std::istringstream ss(ok);
        std::string tok;
        while (std::getline(ss, tok, ',')) parts.push_back(tok);

        std::string ptype = (parts.size() >= 2) ? parts[1] : "?";
        int64_t offset = (parts.size() >= 3) ? std::atoll(parts[2].c_str()) : 0;

        SoapySDR_logf(SOAPY_SDR_INFO,
            "2026SDR: LO=%.0f Hz  type=%s  offset=%+lld Hz  N=%d  M=%lld+%lld/%lld",
            rfHz, ptype.c_str(), (long long)offset,
            p.n, (long long)p.a, (long long)p.b, (long long)p.c);
    } else {
        SoapySDR_logf(SOAPY_SDR_WARNING,
            "2026SDR: unexpected FREQ reply: '%s'", ok.c_str());
    }
}
