// Streaming.cpp — miniaudio I/Q capture + SoapySDR stream API.
//
// miniaudio is the audio backend (single-header, cross-platform).
// #define MINIAUDIO_IMPLEMENTATION must appear in exactly ONE translation unit.
//
// Data flow:
//   PCM1808 (stereo 24-bit) → USB Audio → miniaudio callback
//   → ring buffer [I,Q interleaved float32]
//   → readStream() copies into caller's CF32 buffer.
//
// The ring buffer is protected by _ringMutex / _ringCond.
// The audio callback runs on miniaudio's internal audio thread.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "AudioImpl.hpp"  // full AudioImpl definition (needs miniaudio.h first)
#include "Soapy2026SDR.hpp"

#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Formats.hpp>
#include <chrono>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>



// ─── miniaudio data callback (called on audio thread) ────────────────────────

static void maDataCallback(ma_device *dev, void *output,
                            const void *input, ma_uint32 frameCount)
{
    (void)output;
    if (!input || !dev->pUserData) return;
    auto *self = static_cast<Soapy2026SDR *>(dev->pUserData);
    self->_audioCallback(input, frameCount);
}

// Public: called from maDataCallback on the audio thread
void Soapy2026SDR::_audioCallback(const void *input, unsigned int frameCount)
{
    // input is interleaved S24_3LE (PCM1808 native: 3 bytes per sample, little-endian)
    // Channel 0 = I (left), Channel 1 = Q (right)
    // Each frame = 6 bytes: [I_b0 I_b1 I_b2 Q_b0 Q_b1 Q_b2]
    const uint8_t *src = static_cast<const uint8_t *>(input);

    std::unique_lock<std::mutex> lock(_ringMutex);

    for (unsigned int i = 0; i < frameCount; i++) {
        if (_ringCount >= RING_FRAMES) {
            // Overflow: drop oldest frame
            _ringHead = (_ringHead + 2) % _ring.size();
            _ringCount--;
            _ringOverflow = true;
        }
        // Unpack two S24_3LE samples and sign-extend to 32-bit, then normalize
        // Each sample: byte[0]=LSB, byte[1], byte[2]=MSB (sign bit in bit 23)
        const uint8_t *p = src + i * 6;
        int32_t rawI = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
        if (rawI & 0x800000) rawI |= 0xFF000000;  // sign-extend bit 23
        int32_t rawQ = (int32_t)((uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16));
        if (rawQ & 0x800000) rawQ |= 0xFF000000;
        float I = static_cast<float>(rawI) / 8388608.0f;  // 2^23
        float Q = static_cast<float>(rawQ) / 8388608.0f;
        _ring[_ringTail]     = I;
        _ring[_ringTail + 1] = Q;
        _ringTail = (_ringTail + 2) % _ring.size();
        _ringCount++;
    }

    lock.unlock();
    _ringCond.notify_one();
}

// ─── Audio device setup / teardown ───────────────────────────────────────────

// Scan /proc/asound/cards to find the ALSA short card name for a device
// whose description contains the given search string (case-insensitive).
// Returns e.g. "SDR" for "card 3: SDR [WWU SDR]", or "" if not found.
static std::string _findAlsaCardName(const std::string &needle)
{
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s;
    };
    std::string lcNeedle = toLower(needle);

    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return "";

    char line[256];
    std::string shortName;
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        // Lines look like: " 3 [SDR            ]: USB-Audio - WWU SDR"
        // Short name is between '[' and ']'
        auto lb = l.find('[');
        auto rb = l.find(']');
        if (lb == std::string::npos || rb == std::string::npos) continue;
        // Extract and trim the short name
        std::string candidate = l.substr(lb + 1, rb - lb - 1);
        while (!candidate.empty() && candidate.back() == ' ') candidate.pop_back();
        while (!candidate.empty() && candidate.front() == ' ') candidate.erase(candidate.begin());
        // Check if the whole line contains the needle
        if (toLower(l).find(lcNeedle) != std::string::npos) {
            shortName = candidate;
            break;
        }
    }
    fclose(f);
    return shortName;
}

void Soapy2026SDR::_setupAudio()
{
    AudioImpl *ai = new AudioImpl();

    // Force ALSA backend so we get a direct hw:CARD=X,DEV=0 path.
    // IMPORTANT: we do NOT use miniaudio's device enumeration here.
    // miniaudio's ALSA enumerator converts "hw:CARD=SDR,DEV=0" → ":3,0",
    // which it then opens via dsnoop/hw — but on systems running PipeWire,
    // even these "hw:" opens are intercepted by PipeWire's libasound shim
    // and silently resampled to the server's fixed rate (typically 48 kHz).
    //
    // The only reliable bypass is to pass the hw:CARD=X,DEV=0 string
    // *directly* as the ma_device_id.alsa field so miniaudio calls
    // snd_pcm_open("hw:CARD=SDR,DEV=0", ...) verbatim, which hits the
    // real kernel USB audio driver before PipeWire can intercept it.
    ma_backend backends[] = { ma_backend_alsa };
    ma_context_config ctxCfg = ma_context_config_init();
    if (ma_context_init(backends, 1, &ctxCfg, &ai->context) != MA_SUCCESS) {
        delete ai;
        throw std::runtime_error("2026SDR: failed to initialize miniaudio ALSA context");
    }
    ai->contextOk = true;

    // Build the hw:CARD=X,DEV=0 device ID string directly from /proc/asound/cards.
    // Two-pass: first try the full _audioDeviceName, then fall back to "sdr".
    std::string hwDeviceStr;
    for (int pass = 0; pass < 2 && hwDeviceStr.empty(); pass++) {
        std::string needle = (pass == 0) ? _audioDeviceName : "sdr";
        std::string cardName = _findAlsaCardName(needle);
        if (!cardName.empty()) {
            hwDeviceStr = "hw:CARD=" + cardName + ",DEV=0";
            SoapySDR_logf(SOAPY_SDR_INFO,
                "2026SDR: [pass %d] found ALSA card '%s' → using device '%s'",
                pass, cardName.c_str(), hwDeviceStr.c_str());
        }
    }

    if (hwDeviceStr.empty()) {
        SoapySDR_logf(SOAPY_SDR_WARNING,
            "2026SDR: could not find ALSA card for '%s' in /proc/asound/cards; "
            "falling back to default (may be resampled by PipeWire)",
            _audioDeviceName.c_str());
    }

    // Set the device ID directly — bypasses miniaudio enumeration and PipeWire shim.
    ma_device_id hwId;
    ma_device_id *pDeviceId = nullptr;
    if (!hwDeviceStr.empty()) {
        MA_ZERO_OBJECT(&hwId);
        strncpy(hwId.alsa, hwDeviceStr.c_str(), sizeof(hwId.alsa) - 1);
        pDeviceId = &hwId;
    }

    // Configure capture device.
    // The PCM1808 / RP2040 USB audio descriptor advertises S24_3LE (packed
    // 24-bit, 3 bytes per sample). We request that format explicitly.
    // noAutoResample is belt-and-suspenders in case the hw: path ever goes
    // through the plug layer.
    ma_device_config devCfg = ma_device_config_init(ma_device_type_capture);
    devCfg.capture.pDeviceID      = pDeviceId;
    devCfg.capture.format         = ma_format_s24;   // PCM1808 native: S24_3LE
    devCfg.capture.channels       = 2;               // stereo: L=I, R=Q
    devCfg.sampleRate             = (ma_uint32)_sampleRate;
    devCfg.dataCallback           = maDataCallback;
    devCfg.pUserData              = this;
    devCfg.alsa.noAutoResample    = MA_TRUE;

    if (ma_device_init(&ai->context, &devCfg, &ai->device) != MA_SUCCESS) {
        ma_context_uninit(&ai->context);
        delete ai;
        throw std::runtime_error("2026SDR: failed to open audio capture device '"
                                 + hwDeviceStr + "'");
    }
    ai->deviceOk = true;

    _audio = static_cast<void *>(ai);

    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: audio initialized at %.0f Hz on %s",
                  _sampleRate, hwDeviceStr.empty() ? "default" : hwDeviceStr.c_str());
}

void Soapy2026SDR::_teardownAudio()
{
    if (!_audio) return;
    AudioImpl *ai = static_cast<AudioImpl *>(_audio);


    if (ai->deviceOk) {
        ma_device_stop(&ai->device);
        ma_device_uninit(&ai->device);
    }
    if (ai->contextOk) {
        ma_context_uninit(&ai->context);
    }
    delete ai;
    _audio = nullptr;
}

// ─── SoapySDR Stream API ─────────────────────────────────────────────────────

// We use a simple "stream token" — a non-null sentinel pointer
static int STREAM_TOKEN = 0xA026;

std::vector<std::string> Soapy2026SDR::getStreamFormats(int, size_t) const
{
    return {SOAPY_SDR_CF32, SOAPY_SDR_CS16};
}

std::string Soapy2026SDR::getNativeStreamFormat(int, size_t,
                                                  double &fullScale) const
{
    fullScale = 2147483648.0; // 32-bit int full scale (PCM1808 24-bit in S32)
    return SOAPY_SDR_CS32;
}

SoapySDR::Stream *Soapy2026SDR::setupStream(int dir,
                                              const std::string &format,
                                              const std::vector<size_t> &,
                                              const SoapySDR::Kwargs &)
{
    if (dir != SOAPY_SDR_RX)
        throw std::runtime_error("2026SDR: only RX streaming supported");
    if (format != SOAPY_SDR_CF32 && format != SOAPY_SDR_CS16)
        throw std::runtime_error("2026SDR: unsupported format " + format);

    // Reset ring buffer
    {
        std::lock_guard<std::mutex> lk(_ringMutex);
        _ringHead = _ringTail = _ringCount = 0;
        _ringOverflow = false;
    }

    return reinterpret_cast<SoapySDR::Stream *>(&STREAM_TOKEN);
}

void Soapy2026SDR::closeStream(SoapySDR::Stream *) {}

size_t Soapy2026SDR::getStreamMTU(SoapySDR::Stream *) const
{
    return 4096; // frames per readStream call
}

int Soapy2026SDR::activateStream(SoapySDR::Stream *, int, long long, size_t)
{
    AudioImpl *ai = static_cast<AudioImpl *>(_audio);
    if (!ai || !ai->deviceOk) return SOAPY_SDR_NOT_SUPPORTED;
    if (ma_device_start(&ai->device) != MA_SUCCESS) return SOAPY_SDR_STREAM_ERROR;
    return 0;
}

int Soapy2026SDR::deactivateStream(SoapySDR::Stream *, int, long long)
{
    AudioImpl *ai = static_cast<AudioImpl *>(_audio);
    if (ai && ai->deviceOk) ma_device_stop(&ai->device);
    _ringCond.notify_all(); // wake any blocked readStream
    return 0;
}

int Soapy2026SDR::readStream(SoapySDR::Stream *,
                              void *const *buffs,
                              size_t numElems,
                              int   &flags,
                              long long &timeNs,
                              long  timeoutUs)
{
    flags  = 0;
    timeNs = 0;

    std::unique_lock<std::mutex> lock(_ringMutex);

    // Wait until enough samples are available
    auto waitUntil = std::chrono::steady_clock::now()
                   + std::chrono::microseconds(timeoutUs);
    while (_ringCount < numElems) {
        if (_ringCond.wait_until(lock, waitUntil) == std::cv_status::timeout) {
            if (_ringCount == 0)
                return SOAPY_SDR_TIMEOUT;
            break;  // return partial results rather than timing out entirely
        }
    }

    if (_ringOverflow) {
        flags |= SOAPY_SDR_OVERFLOW;
        _ringOverflow = false;
    }

    size_t toRead = std::min(numElems, _ringCount);
    auto  *dst    = static_cast<float *>(buffs[0]);

    for (size_t i = 0; i < toRead; i++) {
        dst[i * 2 + 0] = _ring[_ringHead];          // I
        dst[i * 2 + 1] = _ring[_ringHead + 1];      // Q
        _ringHead = (_ringHead + 2) % _ring.size();
        _ringCount--;
    }

    return static_cast<int>(toRead);
}
