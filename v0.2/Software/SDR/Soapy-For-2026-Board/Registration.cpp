// Registration.cpp — SoapySDR discovery + factory for the WWU 2026 SDR board.
//
// Discovery strategy:
//   1. Scan serial ports for VID:PID cafe:4011 or cafe:4010 (RP2040 Pico CDC).
//      Falls back to scanning common ACM/usbmodem paths if enumeration fails.
//   2. Report the found port; the audio device ("WWU SDR") is located via
//      miniaudio during device construction.
//
// Usage:
//   SoapySDRUtil --find="driver=2026sdr"
//   SoapySDRUtil --make="driver=2026sdr"

#include "Soapy2026SDR.hpp"
#include "SerialPort.hpp"
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Logger.hpp>
#include <vector>
#include <string>
#include <cstdlib>

// RP2040 Pico CDC VID:PID (both bootloader and application variants)
static const uint16_t PICO_VID  = 0xCAFE;
static const uint16_t PICO_PID1 = 0x4011; // standard firmware
static const uint16_t PICO_PID2 = 0x4010; // alternate PID

// ─── Device discovery ────────────────────────────────────────────────────────

static std::vector<SoapySDR::Kwargs> find2026SDR(const SoapySDR::Kwargs &args)
{
    std::vector<SoapySDR::Kwargs> results;

    // 1. Try VID:PID enumeration for both known PIDs
    std::vector<std::string> ports;
    {
        auto p1 = SerialPort::findPorts(PICO_VID, PICO_PID1);
        auto p2 = SerialPort::findPorts(PICO_VID, PICO_PID2);
        ports.insert(ports.end(), p1.begin(), p1.end());
        ports.insert(ports.end(), p2.begin(), p2.end());
    }

    // 2. If VID/PID enumeration returned nothing (some Linux configurations,
    //    or the udev rules are missing), try a short list of common paths.
    if (ports.empty()) {
#ifdef _WIN32
        for (int i = 1; i <= 20; i++) {
            ports.push_back("COM" + std::to_string(i));
        }
#elif defined(__APPLE__)
        for (int i = 0; i < 8; i++) {
            ports.push_back("/dev/cu.usbmodem" + std::to_string(i));
        }
#else
        for (int i = 0; i < 8; i++) {
            ports.push_back("/dev/ttyACM" + std::to_string(i));
        }
#endif
    }

    // 3. If caller specified a serial_port override, use only that.
    if (args.count("serial_port")) {
        ports.clear();
        ports.push_back(args.at("serial_port"));
    }

    // 4. For each candidate port, try to open it and query the firmware.
    for (const auto &path : ports) {
        SerialPort sp;
        if (!sp.open(path, 115200)) continue;

        // Send Ctrl-C / Ctrl-D to abort any partial command, then ask for VER.
        char reset[] = {0x03, 0x04, 0};
        sp.writeLine(std::string(reset));
        sp.flushInput();

        sp.writeLine("VER");
        std::string reply;
        for (int attempt = 0; attempt < 6; attempt++) {
            reply = sp.readLine(400);
            if (reply.rfind("VER,", 0) == 0) break;
            reply.clear();
        }
        sp.close();

        if (reply.empty()) continue; // not a 2026 board

        // Extract firmware version string after "VER,"
        std::string ver = reply.substr(4);

        SoapySDR::Kwargs info;
        info["driver"]      = "2026sdr";
        info["serial_port"] = path;
        info["label"]       = "WWU 2026 SDR (" + path + ")";
        info["version"]     = ver;

        // Apply user filter on label
        if (args.count("label") &&
            info["label"].find(args.at("label")) == std::string::npos)
            continue;

        SoapySDR_logf(SOAPY_SDR_INFO, "Found WWU 2026 SDR on %s (fw: %s)",
                      path.c_str(), ver.c_str());
        results.push_back(info);
    }

    return results;
}

// ─── Device factory ──────────────────────────────────────────────────────────

static SoapySDR::Device *make2026SDR(const SoapySDR::Kwargs &args)
{
    return new Soapy2026SDR(args);
}

// ─── Registration ────────────────────────────────────────────────────────────

static SoapySDR::Registry register2026SDR(
    "2026sdr", &find2026SDR, &make2026SDR, SOAPY_SDR_ABI_VERSION);
