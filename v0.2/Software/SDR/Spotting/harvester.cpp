#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <liquid/liquid.h>
#include <sndfile.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;
constexpr unsigned INPUT_RATE = 96000, OUTPUT_RATE = 12000, DECIM = 8;
constexpr size_t INPUT_BLOCK = 4096, OUTPUT_BLOCK = INPUT_BLOCK / DECIM;
std::atomic<bool> running{true};

struct ModeConfig { std::string mode; int offset; int audioIf; };
struct SdrConfig { std::string id, serial, audio; double center; std::vector<ModeConfig> modes; };

static std::chrono::system_clock::time_point utcFrameStart(const std::string &mode, std::chrono::system_clock::time_point now) {
    using namespace std::chrono;
    const auto half = duration_cast<milliseconds>(now.time_since_epoch());
    const int64_t periodMs = mode == "FT8" ? 15000 : mode == "FT4" ? 7500 : 120000;
    return system_clock::time_point(milliseconds((half.count() / periodMs) * periodMs));
}

static std::string timestamp(std::chrono::system_clock::time_point t) {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(t.time_since_epoch()).count() % 1000;
    const auto tt = std::chrono::system_clock::to_time_t(t); std::tm tm{}; gmtime_r(&tt, &tm);
    std::ostringstream out; out << std::put_time(&tm, "%Y%m%dT%H%M%S");
    if (ms != 0) out << '.' << std::setfill('0') << std::setw(3) << ms;
    out << 'Z'; return out.str();
}

static size_t frameSamples(const std::string &mode) {
    return mode == "FT8" ? 15u * OUTPUT_RATE : mode == "FT4" ? 15u * OUTPUT_RATE / 2u : 120u * OUTPUT_RATE;
}

static std::chrono::milliseconds framePeriod(const std::string &mode) {
    return std::chrono::milliseconds(mode == "FT8" ? 15000 : mode == "FT4" ? 7500 : 120000);
}

class WavFrame {
public:
    WavFrame(const fs::path &dir, const SdrConfig &sdr, const ModeConfig &mode, std::chrono::system_clock::time_point start)
        : final_(dir / (sdr.id + "_" + mode.mode + "_" + timestamp(start) + ".wav")), part_(final_.string() + ".part") {
        SF_INFO info{}; info.samplerate = OUTPUT_RATE; info.channels = 1; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        file_ = sf_open(part_.c_str(), SFM_WRITE, &info);
        if (!file_) throw std::runtime_error("sf_open failed for " + part_.string());
        pcm_.reserve(OUTPUT_BLOCK);
    }
    ~WavFrame() { discard(); }
    void write(const liquid_float_complex *samples, size_t count) {
        pcm_.resize(count);
        const float GAIN = 20.0f; // BOOST AUDIO by ~26 dB to utilize 16-bit dynamic range
        for (size_t i = 0; i < count; ++i) {
            const float real = std::clamp(static_cast<float>(std::real(samples[i])) * GAIN, -1.0f, 1.0f);
            pcm_[i] = static_cast<short>(std::lrint(real * 32767.0f));
        }
        if (sf_write_short(file_, pcm_.data(), static_cast<sf_count_t>(count)) != static_cast<sf_count_t>(count))
            throw std::runtime_error("libsndfile write failed for " + part_.string());
    }
    void close() {
        if (!file_) return;
        sf_write_sync(file_); sf_close(file_); file_ = nullptr;
        std::error_code ec; fs::rename(part_, final_, ec);
        if (ec) std::cerr << "rename failed: " << ec.message() << '\n';
    }
    void discard() {
        if (file_) { sf_close(file_); file_ = nullptr; }
        std::error_code ec; fs::remove(part_, ec);
    }
private:
    fs::path final_, part_; SNDFILE *file_ = nullptr; std::vector<short> pcm_;
};

struct Branch {
    ModeConfig mode; nco_crcf nco{}; firdecim_crcf decim{};
    std::unique_ptr<WavFrame> wav; std::chrono::system_clock::time_point frame{};
    size_t samples = 0; bool waitingForBoundary = true; 
    size_t blockCount = 0; // Track blocks for periodic RMS reporting
};

static void destroyBranches(std::vector<Branch> &branches) noexcept {
    for (auto &branch : branches) {
        branch.wav.reset();
        if (branch.nco) { nco_crcf_destroy(branch.nco); branch.nco = nullptr; }
        if (branch.decim) { firdecim_crcf_destroy(branch.decim); branch.decim = nullptr; }
    }
}

static void runSdr(const SdrConfig &cfg, const fs::path &outputDir) {
    constexpr auto retryDelay = std::chrono::seconds(5);

    // Each radio retries independently. A missing or disconnected board does
    // not stop other radios, and plugging it in later allows recovery without
    // restarting the harvester.
    while (running) {
        SoapySDR::Device *dev = nullptr;
        SoapySDR::Stream *stream = nullptr;
        bool streamActive = false;
        iirfilt_crcf dcblock = nullptr;
        std::vector<Branch> branches;

        auto cleanup = [&]() noexcept {
            if (streamActive && dev) {
                try { dev->deactivateStream(stream); } catch (...) {}
                streamActive = false;
            }
            if (stream && dev) {
                try { dev->closeStream(stream); } catch (...) {}
                stream = nullptr;
            }
            if (dev) {
                try { SoapySDR::Device::unmake(dev); } catch (...) {}
                dev = nullptr;
            }
            destroyBranches(branches);
            if (dcblock) { iirfilt_crcf_destroy(dcblock); dcblock = nullptr; }
        };

        try {
            SoapySDR::Kwargs args; args["driver"] = "2026sdr"; args["serial_port"] = cfg.serial; args["audio_label"] = cfg.audio;
            dev = SoapySDR::Device::make(args);
            if (!dev) throw std::runtime_error("unable to create Soapy device");
            dev->setSampleRate(SOAPY_SDR_RX, 0, INPUT_RATE); dev->setFrequency(SOAPY_SDR_RX, 0, cfg.center);
            stream = dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
            if (!stream) throw std::runtime_error("unable to create Soapy stream");
            dev->activateStream(stream); streamActive = true;

            // Create the DC blocker only after the radio has opened, so a
            // missing device does not allocate DSP state on every retry.
            dcblock = iirfilt_crcf_create_dc_blocker(0.0005f);
            if (!dcblock) throw std::runtime_error("unable to create DC blocker");
        
        std::vector<float> taps(65); std::vector<liquid_float_complex> input(INPUT_BLOCK), mixed(INPUT_BLOCK), output(OUTPUT_BLOCK);
        liquid_float_complex carry[DECIM]{}; size_t carryCount = 0;
        
        // 2. Anti-Aliasing Filter Fix: Max cutoff for M=8 is 0.0625. Set to 0.05f to prevent foldover.
        liquid_firdes_kaiser(taps.size(), 0.05f, 60.0f, 0.0f, taps.data());
        float dcGain = 0.0f;
        for (const float tap : taps) dcGain += tap;
        if (std::abs(dcGain) < 1.0e-6f) throw std::runtime_error("invalid decimator coefficients");
        for (float &tap : taps) tap /= dcGain;
        
        branches.reserve(cfg.modes.size());
        for (const auto &m : cfg.modes) {
            Branch b; b.mode = m; b.nco = nco_crcf_create(LIQUID_NCO); b.decim = firdecim_crcf_create(DECIM, taps.data(), taps.size());
            if (!b.nco || !b.decim) throw std::runtime_error("unable to create DSP branch for " + m.mode);
            firdecim_crcf_set_scale(b.decim, 1.0f);
            const double ncoHz = static_cast<double>(m.audioIf - m.offset);
            nco_crcf_set_frequency(b.nco, static_cast<float>(2.0 * M_PI * ncoHz / INPUT_RATE));
            std::cerr << cfg.id << " " << m.mode << ": RF offset=" << m.offset
                      << " Hz, audio IF=" << m.audioIf << " Hz, NCO=" << ncoHz << " Hz\n";
            branches.push_back(std::move(b));
        }

        while (running) {
            int flags = 0; long long timeNs = 0;
            void *buffers[] = {input.data() + carryCount};
            const int requested = static_cast<int>(INPUT_BLOCK - carryCount);
            const int got = dev->readStream(stream, buffers, requested, flags, timeNs, 2000000);
            if (got == SOAPY_SDR_TIMEOUT) continue;
            if (got < 0) throw std::runtime_error("readStream error " + std::to_string(got));
            
            const size_t total = carryCount + static_cast<size_t>(got);
            
            // 3. Remove DC Offset BEFORE doing any mixing!
            iirfilt_crcf_execute_block(dcblock, input.data(), static_cast<unsigned>(total), input.data());

            for (auto &b : branches) {
                b.blockCount++;
                nco_crcf_mix_block_down(b.nco, input.data(), mixed.data(), static_cast<unsigned>(total));
                const size_t usable = (total / DECIM) * DECIM;
                const size_t outCount = usable / DECIM;
                firdecim_crcf_execute_block(b.decim, mixed.data(), static_cast<unsigned>(outCount), output.data());
                
                // 4. Print RMS periodically (every ~20 seconds) so we bypass the startup zeros
                if (b.blockCount % 500 == 10 && outCount > 0) {
                    double inputPower = 0.0, outputPower = 0.0;
                    for (size_t i = 0; i < total; ++i) inputPower += std::norm(input[i]);
                    for (size_t i = 0; i < outCount; ++i) outputPower += std::norm(output[i]);
                    const double inputRms = std::sqrt(inputPower / total);
                    const double outputRms = std::sqrt(outputPower / outCount);
                    std::cerr << cfg.id << " " << b.mode.mode << ": input RMS=" << inputRms
                              << ", WAV real RMS=" << (outputRms / std::sqrt(2.0)) * 20.0f << "\n";
                }

                const auto now = std::chrono::system_clock::now();
                if (b.waitingForBoundary) {
                    if (b.frame.time_since_epoch().count() == 0)
                        b.frame = utcFrameStart(b.mode.mode, now) + framePeriod(b.mode.mode);
                    if (now < b.frame) continue;
                    b.waitingForBoundary = false;
                    b.wav = std::make_unique<WavFrame>(outputDir, cfg, b.mode, b.frame);
                    b.samples = 0;
                }
                
                size_t consumed = 0;
                const size_t samplesPerFrame = frameSamples(b.mode.mode);
                while (consumed < outCount) {
                    if (!b.wav) b.wav = std::make_unique<WavFrame>(outputDir, cfg, b.mode, b.frame);
                    const size_t take = std::min(outCount - consumed, samplesPerFrame - b.samples);
                    b.wav->write(output.data() + consumed, take); consumed += take; b.samples += take;
                    if (b.samples == samplesPerFrame) {
                        b.wav->close(); b.wav.reset(); b.samples = 0; b.frame += framePeriod(b.mode.mode);
                    }
                }
            }
            carryCount = total - (total / DECIM) * DECIM;
            for (size_t i = 0; i < carryCount; ++i) carry[i] = input[total - carryCount + i];
            for (size_t i = 0; i < carryCount; ++i) input[i] = carry[i];
        }
        cleanup();
        } catch (const std::exception &e) {
            cleanup();
            if (running) {
                std::cerr << cfg.id << ": " << e.what()
                          << "; retrying in " << retryDelay.count() << " seconds\n";
                // Use short sleeps so SIGINT/SIGTERM stops promptly even
                // while a radio is absent.
                for (int i = 0; i < 50 && running; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}

static void stop(int) { running = false; }

int main(int argc, char **argv) {
    const fs::path configPath = argc > 1 ? argv[1] : "config.json"; std::ifstream in(configPath);
    if (!in) { std::cerr << "Cannot open " << configPath << '\n'; return 1; }
    json root; in >> root;
    const fs::path outputDir(root.value("output_dir", "/home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures"));
    fs::create_directories(outputDir);
    std::vector<SdrConfig> configs;
    for (const auto &x : root.at("sdrs")) {
        SdrConfig s{x.at("sdr_id"), x.at("serial_port"), x.at("audio_label"), x.at("center_freq"), {}};
        for (const auto &m : x.at("modes")) {
            s.modes.push_back({m.at("mode"), m.at("offset"), m.value("audio_if", 1500)});
        }
        configs.push_back(std::move(s));
    }
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop); std::vector<std::thread> threads;
    for (const auto &s : configs) {
        threads.emplace_back(runSdr, std::cref(s), outputDir);
    }
    for (auto &t : threads) t.join();
}
