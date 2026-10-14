#include "SynthEngine.h"
#include "MidiParser.h"
#include "Parameters.h"
#include "Presets.h"
#include "../src/usb_host_midi.h"
#include "../src/knob_page.h"
#include "sim_util.h"
#include "sim_keyboard.h"

#include <portaudio.h>
#include <rtmidi/RtMidi.h>

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <mutex>

using SimUtil::containsCaseInsensitive;

// Global run flag for graceful shutdown
static std::atomic<bool> g_running{true};

void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

// Saved terminal state, restored on normal exit, exit(), SIGINT/SIGTERM (via the main loop)
// and fatal signals (directly from the handler; tcsetattr is async-signal-safe)
static struct termios g_origTermios;
static volatile sig_atomic_t g_termiosSaved = 0;

static void restoreTerminal() {
    if (g_termiosSaved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_origTermios);
        g_termiosSaved = 0;
    }
}

static void fatalSignalHandler(int sig) {
    restoreTerminal();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// Keyboard play mode: no echo, no line buffering. ISIG stays on so Ctrl+C still raises SIGINT.
class TerminalRawMode {
public:
    TerminalRawMode() {
        if (tcgetattr(STDIN_FILENO, &g_origTermios) != 0) return;
        struct termios raw = g_origTermios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        g_termiosSaved = 1;
        static bool atexitRegistered = false;
        if (!atexitRegistered) {
            std::atexit(restoreTerminal);
            for (int sig : {SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL}) std::signal(sig, fatalSignalHandler);
            atexitRegistered = true;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    ~TerminalRawMode() { restoreTerminal(); }
    TerminalRawMode(const TerminalRawMode&) = delete;
    TerminalRawMode& operator=(const TerminalRawMode&) = delete;
};

// Application state shared between MIDI and Audio callbacks
struct SimulatorContext {
    Synth::SynthEngine engine;
    HardwareDriver::UsbHostMidi usbMidi;
    std::string connectedPortName{"None"};
    std::mutex displayMutex;
    int swOutIdx{-1};
    int swInIdx{-1};
    int pluginOutIdx{-1};
    std::vector<int> midiOutIndices;
    std::unique_ptr<RtMidiOut> swOutPort;
    std::unique_ptr<RtMidiOut> pluginOutPort;
    std::vector<std::unique_ptr<RtMidiOut>> activeOutPorts;
    std::vector<unsigned char> basePreset;
    // Serialises the input producers (RtMidi callback thread, computer keyboard / main loop) into
    // usbMidi and the engine's event queue. The audio thread never takes it.
    std::mutex inputMutex;
};

// Send MIDI CC feedback messages to update physical rotary encoder positions on the controller
static void sendMidiCcFeedback(SimulatorContext& ctx, const Synth::KnobBank& bank) {
    try {
        for (auto& midiOut : ctx.activeOutPorts) {
            if (!midiOut) continue;
            for (size_t k = 0; k < 8; ++k) {
                std::vector<unsigned char> ccMsg = {
                    0xB0,                  // Channel 1 Control Change
                    bank.ccs[k],           // CC Number
                    bank.defaults[k]       // Default Value [0..127]
                };
                midiOut->sendMessage(&ccMsg);
            }
        }
    } catch (...) {}
}

// Upload synth parameter names and bank definition to MPK Mini IV display via SynthEngine's MpkMiniMk4::DisplayDriver
static bool uploadMpkDisplayPreset(SimulatorContext& ctx, const Synth::Preset& p, bool isShift) {
    std::lock_guard<std::mutex> lock(ctx.displayMutex);
    if (!ctx.engine.getMpkDisplay().isConnected()) {
        return false;
    }

    const auto& bank = isShift ? p.bankB : p.bankA;
    // applyPreset() already updates the OLED screen and pre-seeds knobs via updateMpkMiniDisplay().
    sendMidiCcFeedback(ctx, bank);
    return true;
}

// Initial probe to read base preset template from MPK Mini IV and wire callbacks into SynthEngine
static bool initializeMpkDisplayTemplate(SimulatorContext& ctx) {
    try {
        RtMidiIn midiIn;
        RtMidiOut midiOut;

        ctx.swInIdx = -1;
        ctx.swOutIdx = -1;
        ctx.pluginOutIdx = -1;
        ctx.midiOutIndices.clear();

        for (unsigned int i = 0; i < midiIn.getPortCount(); ++i) {
            std::string n = midiIn.getPortName(i);
            if (ctx.swInIdx < 0 && n.find("Software Control") != std::string::npos) ctx.swInIdx = i;
        }
        for (unsigned int i = 0; i < midiOut.getPortCount(); ++i) {
            std::string n = midiOut.getPortName(i);
            if (ctx.swOutIdx < 0 && n.find("Software Control") != std::string::npos) {
                ctx.swOutIdx = i;
            }
            if (ctx.pluginOutIdx < 0 && n.find("Plugin Port") != std::string::npos) {
                ctx.pluginOutIdx = i;
            }
            if (n.find("MIDI Port") != std::string::npos ||
                n.find("DAW Port") != std::string::npos) {
                ctx.midiOutIndices.push_back(i);
            }
        }

        if (ctx.swInIdx < 0 || ctx.swOutIdx < 0) return false;

        midiIn.openPort(ctx.swInIdx);
        midiOut.openPort(ctx.swOutIdx);
        midiIn.ignoreTypes(false, false, false);

        std::vector<unsigned char> received;
        midiIn.setCallback([](double, std::vector<unsigned char>* msg, void* userData) {
            auto* vec = static_cast<std::vector<unsigned char>*>(userData);
            if (msg && msg->size() == 284 && (*msg)[4] == 0x67) {
                *vec = *msg;
            }
        }, &received);

        // Query active RAM preset: F0 47 7F 5D 66 00 00 00 F7
        std::vector<unsigned char> query = {0xF0, 0x47, 0x7F, 0x5D, 0x66, 0x00, 0x00, 0x00, 0xF7};
        midiOut.sendMessage(&query);

        for (int i = 0; i < 20; ++i) {
            if (!received.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }

        if (received.empty()) return false;
        ctx.basePreset = received;

        // Open permanent output ports
        if (ctx.swOutIdx >= 0) {
            ctx.swOutPort = std::make_unique<RtMidiOut>();
            ctx.swOutPort->openPort(ctx.swOutIdx);
        }
        if (ctx.pluginOutIdx >= 0) {
            ctx.pluginOutPort = std::make_unique<RtMidiOut>();
            ctx.pluginOutPort->openPort(ctx.pluginOutIdx);
        }
        for (int outIdx : ctx.midiOutIndices) {
            auto port = std::make_unique<RtMidiOut>();
            port->openPort(outIdx);
            ctx.activeOutPorts.push_back(std::move(port));
        }

        // Configure SynthEngine's integrated MpkMiniMk4::DisplayDriver
        ctx.engine.getMpkDisplay().setBasePresetTemplate(ctx.basePreset);
        auto pluginSender = [&ctx](const uint8_t* data, size_t len) {
            if (ctx.pluginOutPort) {
                std::vector<unsigned char> msg(data, data + len);
                ctx.pluginOutPort->sendMessage(&msg);
            }
        };
        auto swSender = [&ctx](const uint8_t* data, size_t len) {
            if (ctx.swOutPort) {
                std::vector<unsigned char> msg(data, data + len);
                ctx.swOutPort->sendMessage(&msg);
            }
        };
        ctx.engine.setMpkDisplaySendCallbacks(pluginSender, swSender);
        ctx.usbMidi.getControlSurface().setDisplayDriver(&ctx.engine.getMpkDisplay());
        ctx.usbMidi.getControlSurface().setPluginPortSender(pluginSender);
        ctx.usbMidi.getControlSurface().setSoftwareControlPortSender(swSender);


        return true;
    } catch (...) {
        return false;
    }
}

// PortAudio stream callback (44.1 kHz, 16-bit stereo, 128 frames)
static int paCallback(const void* inputBuffer, void* outputBuffer,
                      unsigned long framesPerBuffer,
                      const PaStreamCallbackTimeInfo* timeInfo,
                      PaStreamCallbackFlags statusFlags,
                      void* userData) {
    (void)inputBuffer;
    (void)timeInfo;
    (void)statusFlags;

    auto* ctx = static_cast<SimulatorContext*>(userData);
    int16_t* out = static_cast<int16_t*>(outputBuffer);

    ctx->engine.processBlock(out, framesPerBuffer);
    return paContinue;
}

// One MIDI message into the unified pipeline via UsbHostMidi (same as the ESP32-S3).
// Callers must hold ctx.inputMutex. `source` labels the console log ("MIDI IN" or "KEYS").
static void routeMidiMessage(SimulatorContext& ctx, const std::vector<unsigned char>& message, const char* source) {
    if (message.empty()) return;

    uint8_t status = message[0];
    uint8_t msgType = status & 0xF0;
    uint8_t channel = (status & 0x0F) + 1;

    // Route through the identical UsbHostMidi pipeline used on the ESP32-S3.
    // This handles knob Bank A/B resolution, Shift gating, encoder navigation,
    // +/- preset stepping, Bank Left/Right page cycling, and MPK display updates.
    if (status == 0xF0) {
        ctx.usbMidi.processSysEx(message.data(), message.size());
        return;
    }

    if (message.size() >= 3) {
        auto evt = ctx.usbMidi.processMidiMessage(status, message[1], message[2]);
        (void)evt; // Event logging handled by callbacks below
    } else if (message.size() == 2) {
        // Program Change (0xC0) has only 2 data bytes
        ctx.usbMidi.processMidiMessage(status, message[1], 0);
    } else if (message.size() == 1 && status >= 0xF8) {
        // System Real-Time (Clock, Start, Stop) — single byte
        ctx.usbMidi.processMidiMessage(status, 0, 0);
    }

    // Console diagnostic logging (display only, no further processing)
    if (msgType == 0x90 && message.size() >= 3) {
        uint8_t note = message[1];
        uint8_t vel = message[2];
        if (vel > 0) {
            std::cout << "\r[" << source << "] Note On : Ch " << (int)channel
                      << " | Note " << (int)note << " " << SimUtil::noteName(note)
                      << " (" << std::fixed << std::setprecision(1)
                      << Synth::ParameterScaler::midiNoteToHz(note) << " Hz)"
                      << " | Vel " << (int)vel << "    \n" << std::flush;
        } else {
            std::cout << "\r[" << source << "] Note Off: Ch " << (int)channel
                      << " | Note " << (int)note << "    \n" << std::flush;
        }
    } else if (msgType == 0x80 && message.size() >= 3) {
        uint8_t note = message[1];
        std::cout << "\r[" << source << "] Note Off: Ch " << (int)channel
                  << " | Note " << (int)note << "    \n" << std::flush;
    } else if (msgType == 0xB0 && message.size() >= 3) {
        uint8_t cc = message[1];
        uint8_t val = message[2];

        // Show resolved knob name from the active bank
        const auto& bank = ctx.engine.getActiveKnobBank();
        std::string ccName = "CC " + std::to_string(cc);
        for (int k = 0; k < 8; ++k) {
            if (bank.ccs[k] == cc) {
                ccName = bank.labels[k];
                break;
            }
        }

        std::cout << "\r[" << source << "] " << std::setw(8) << ccName << ": " << (int)val << "    \n" << std::flush;
    } else if (msgType == 0xE0 && message.size() >= 3) {
        int16_t bend = static_cast<int16_t>(message[1] | (message[2] << 7));
        float semitones = ((bend - 8192) / 8192.0f) * 2.0f;
        std::cout << "\r[" << source << "] Pitch Bend: " << std::showpos << std::setprecision(2)
                  << semitones << " semitones" << std::noshowpos << "    \n" << std::flush;
    }
}

// RtMidi input callback (RtMidi's thread)
static void midiCallback(double timeStamp, std::vector<unsigned char>* message, void* userData) {
    (void)timeStamp;
    if (!message || message->empty()) return;
    auto* ctx = static_cast<SimulatorContext*>(userData);
    std::lock_guard<std::mutex> lock(ctx->inputMutex);
    routeMidiMessage(*ctx, *message, "MIDI IN");
}

// ----------------------------------------------------
// Command line
// ----------------------------------------------------
struct SimOptions {
    bool testMode{false};
    bool keyboard{true};
    bool listDevices{false};
    long preset{1};                // 1-based slot
    std::string audioDevice;       // index or name substring; empty = system default
    std::string midiPort;          // extra input port name substring
    long bufferFrames{static_cast<long>(Synth::SynthConfig::BLOCK_SIZE)};
};

static constexpr long kMinBufferFrames = 16;
static constexpr long kMaxBufferFrames = 4096;

static void printUsage(std::ostream& os) {
    os << "Usage: synth_sim [options]\n"
          "\n"
          "Desktop simulator: SynthEngine -> PortAudio, played from an Akai MPK Mini MK4 (auto-detected),\n"
          "the 'SynthEngine Virtual In' MIDI port, or the computer keyboard.\n"
          "\n"
          "  --preset N              Start on preset slot N (1.." << Synth::TOTAL_PRESET_COUNT << ", default 1)\n"
          "  --audio-device D        Output device by index or name substring (see --list-devices)\n"
          "  --midi-port S           Also listen on MIDI inputs whose name contains S (case-insensitive)\n"
          "  --buffer FRAMES         Audio buffer size, " << kMinBufferFrames << ".." << kMaxBufferFrames
       << " (default " << Synth::SynthConfig::BLOCK_SIZE << ")\n"
          "  --list-devices          List audio outputs and MIDI inputs, then exit\n"
          "  --no-keyboard           Disable computer-keyboard play mode\n"
          "  --test                  Headless smoke test: load presets, toggle bank, exit\n"
          "  --help                  Show this help\n"
          "\n"
          "Keyboard play mode is on when stdin is a terminal; press ? inside the simulator for keys.\n";
}

static int usageError(const std::string& msg) {
    std::cerr << "synth_sim: " << msg << "\n\n";
    printUsage(std::cerr);
    return 2;
}

// Returns -1 to continue, otherwise the process exit code
static int parseArgs(int argc, char* argv[], SimOptions& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;
        bool hasInlineValue = false;
        const size_t eq = arg.find('=');
        if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
            value = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
            hasInlineValue = true;
        }
        auto takeValue = [&](std::string& dst) {
            if (hasInlineValue) { dst = value; return !dst.empty(); }
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };

        const bool isSwitch = arg == "--help" || arg == "-h" || arg == "--test" ||
                              arg == "--no-keyboard" || arg == "--list-devices";
        if (isSwitch && hasInlineValue) return usageError(arg + " takes no value");

        std::string v;
        if (arg == "--help" || arg == "-h") { printUsage(std::cout); return 0; }
        else if (arg == "--test")         opt.testMode = true;
        else if (arg == "--no-keyboard")  opt.keyboard = false;
        else if (arg == "--list-devices") opt.listDevices = true;
        else if (arg == "--preset") {
            if (!takeValue(v) || !SimUtil::parseInt(v, opt.preset) ||
                opt.preset < 1 || opt.preset > static_cast<long>(Synth::TOTAL_PRESET_COUNT))
                return usageError("--preset needs a slot number 1.." + std::to_string(Synth::TOTAL_PRESET_COUNT));
        }
        else if (arg == "--buffer") {
            if (!takeValue(v) || !SimUtil::parseInt(v, opt.bufferFrames) ||
                opt.bufferFrames < kMinBufferFrames || opt.bufferFrames > kMaxBufferFrames)
                return usageError("--buffer needs a frame count " + std::to_string(kMinBufferFrames) +
                                  ".." + std::to_string(kMaxBufferFrames));
        }
        else if (arg == "--audio-device") { if (!takeValue(opt.audioDevice)) return usageError("--audio-device needs an index or name"); }
        else if (arg == "--midi-port")    { if (!takeValue(opt.midiPort)) return usageError("--midi-port needs a name substring"); }
        else return usageError("unknown option '" + std::string(argv[i]) + "'");
    }
    return -1;
}

// Auto-bound MPK Mini MK4 performance ports
static bool isMpkPerformancePort(const std::string& portName) {
    return containsCaseInsensitive(portName, "MPK") &&
           (containsCaseInsensitive(portName, "MIDI Port") ||
            containsCaseInsensitive(portName, "DAW Port") ||
            containsCaseInsensitive(portName, "Plugin Port"));
}

static int listDevices() {
    PaError paErr = Pa_Initialize();
    if (paErr != paNoError) {
        std::cerr << "[PortAudio] Initialization error: " << Pa_GetErrorText(paErr) << "\n";
        return 1;
    }
    const PaDeviceIndex defaultOut = Pa_GetDefaultOutputDevice();
    std::cout << "Audio outputs (--audio-device <index|name>):\n";
    for (PaDeviceIndex d = 0; d < Pa_GetDeviceCount(); ++d) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(d);
        if (!info || info->maxOutputChannels < 1) continue;
        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        std::cout << "  [" << d << "] " << info->name << "  (" << (api ? api->name : "?") << ", "
                  << info->maxOutputChannels << " ch)" << (d == defaultOut ? "  <- default" : "") << "\n";
    }
    Pa_Terminate();

    std::cout << "\nMIDI inputs (--midi-port <substring>):\n";
    try {
        RtMidiIn probe;
        const unsigned int n = probe.getPortCount();
        if (n == 0) std::cout << "  (none)\n";
        for (unsigned int i = 0; i < n; ++i) {
            const std::string name = probe.getPortName(i);
            std::cout << "  [" << i << "] " << name << (isMpkPerformancePort(name) ? "  <- auto (MPK Mini MK4)" : "") << "\n";
        }
    } catch (const RtMidiError& e) {
        std::cout << "  (RtMidi error: " << e.getMessage() << ")\n";
    }
    return 0;
}

// Resolves --audio-device: a device index, or the first stereo output whose name contains the text
static PaDeviceIndex resolveAudioDevice(const std::string& spec) {
    if (spec.empty()) return Pa_GetDefaultOutputDevice();
    long idx = 0;
    if (SimUtil::parseInt(spec, idx)) {
        if (idx < 0 || idx >= Pa_GetDeviceCount()) return paNoDevice;
        const PaDeviceInfo* info = Pa_GetDeviceInfo(static_cast<PaDeviceIndex>(idx));
        return (info && info->maxOutputChannels >= 2) ? static_cast<PaDeviceIndex>(idx) : paNoDevice;
    }
    for (PaDeviceIndex d = 0; d < Pa_GetDeviceCount(); ++d) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(d);
        if (info && info->maxOutputChannels >= 2 && containsCaseInsensitive(info->name, spec)) return d;
    }
    return paNoDevice;
}

static uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void routeKeyboardMidi(SimulatorContext& ctx, const std::vector<SimKeyboard::MidiOut>& msgs) {
    if (msgs.empty()) return;
    std::lock_guard<std::mutex> lock(ctx.inputMutex);
    for (const auto& m : msgs) {
        routeMidiMessage(ctx, {m.status, m.data1, m.data2}, "KEYS");
    }
}

static void printPresetList() {
    const auto& presets = Synth::getPresets();
    std::cout << "Presets (--preset N; switch with , . [ ] or the MPK Mini MK4 SELECT knob / < > keys):\n";
    for (size_t i = 0; i < Synth::FACTORY_PRESET_COUNT; ++i) {
        std::cout << "  [" << std::setw(2) << (i + 1) << "] " << std::left << std::setw(16) << presets[i].name
                  << std::right << " " << presets[i].description << "\n";
    }
    std::cout << "  [" << (Synth::FACTORY_PRESET_COUNT + 1) << ".." << Synth::TOTAL_PRESET_COUNT
              << "] User slots (loaded via BLE/SysEx)\n\n";
}

int main(int argc, char* argv[]) {
    SimOptions opts;
    if (int rc = parseArgs(argc, argv, opts); rc >= 0) return rc;
    if (opts.listDevices) return listDevices();

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "======================================================\n";
    std::cout << "  Multi-Synth Simulator (Juno, Moog, Prophet, 303...)\n";
    std::cout << "======================================================\n\n";

    SimulatorContext ctx;
    ctx.engine.init(Synth::SynthConfig::DEFAULT_SAMPLE_RATE);
    // RtMidi callbacks and the PortAudio callback run on different threads
    ctx.engine.setDeferredEvents(true);
    ctx.usbMidi.init(ctx.engine);

    // Setup onDisplayUpdate listener to update MPK display and print info
    ctx.engine.setOnDisplayUpdate([&ctx](const Synth::Preset& p, bool isShift) {
        uploadMpkDisplayPreset(ctx, p, isShift);
        const auto& bank = isShift ? p.bankB : p.bankA;

        std::cout << "\n======================================================\n";
        std::cout << ">>> [SYNTH ACTIVE]: " << p.name << "  "
                  << (isShift ? "(BANK B / SHIFT)" : "(BANK A / PRIMARY)") << "\n";
        std::cout << "    " << p.description << "\n";
        std::cout << "------------------------------------------------------\n";
        std::cout << "  Active Knob Assignments (with Defaults):\n";
        for (int k = 0; k < 8; ++k) {
            std::cout << "  [K" << (k+1) << "] " << std::setw(8) << bank.labels[k]
                      << " (CC " << std::setw(2) << (int)bank.ccs[k]
                      << " = default " << std::setw(3) << (int)bank.defaults[k] << ")\n";
        }
        std::cout << "======================================================\n\n" << std::flush;
    });

    // ----------------------------------------------------
    // 1. Initialize MPK Mini Display Template
    // ----------------------------------------------------
    std::cout << "[Hardware Feedback] Initializing MPK Mini MK4 display...\n";
    if (initializeMpkDisplayTemplate(ctx)) {
        std::cout << "[Hardware Feedback] SUCCESS: Linked to MPK Mini MK4 Software Control Port!\n";
    } else {
        std::cout << "[Hardware Feedback] (Software Control Port offline or in standby; continuing)\n";
    }

    ctx.usbMidi.selectPreset(static_cast<uint8_t>(opts.preset - 1));

    // ----------------------------------------------------
    // 2. Initialize RtMidi Inputs (MPK MIDI / DAW / Plugin ports, plus --midi-port matches)
    // ----------------------------------------------------
    std::vector<std::unique_ptr<RtMidiIn>> midiInputs;
    bool midiPortMatched = false;
    try {
        RtMidiIn probeIn;
        unsigned int nPorts = probeIn.getPortCount();
        std::cout << "[RtMidi] Detected " << nPorts << " MIDI input port(s):\n";

        for (unsigned int i = 0; i < nPorts; ++i) {
            std::string portName = probeIn.getPortName(i);
            std::cout << "  [" << i << "] " << portName << "\n";

            const bool userMatch = !opts.midiPort.empty() && containsCaseInsensitive(portName, opts.midiPort);
            midiPortMatched = midiPortMatched || userMatch;
            if (isMpkPerformancePort(portName) || userMatch) {
                auto input = std::make_unique<RtMidiIn>();
                input->openPort(i);
                input->setCallback(&midiCallback, &ctx);
                input->ignoreTypes(false, false, false);
                std::cout << "      --> Bound listener to: " << portName << "\n";
                midiInputs.push_back(std::move(input));
            }
        }
    } catch (const RtMidiError& e) {
        std::cout << "[RtMidi] Warning: " << e.getMessage() << "\n";
    }

    if (!opts.midiPort.empty() && !midiPortMatched) {
        std::cerr << "[RtMidi] No MIDI input matches --midi-port '" << opts.midiPort
                  << "' (see synth_sim --list-devices)\n";
        return 1;
    }
    if (midiInputs.empty()) {
        std::cout << "[RtMidi] No Akai MPK Mini MK4 ports detected. Connect MPK Mini MK4 via USB.\n";
    }

    // Always create a virtual port for CLI / Software testing
    try {
        auto virtInput = std::make_unique<RtMidiIn>();
        virtInput->openVirtualPort("SynthEngine Virtual In");
        virtInput->setCallback(&midiCallback, &ctx);
        virtInput->ignoreTypes(false, false, false);
        std::cout << "[RtMidi] Opened virtual input port: 'SynthEngine Virtual In'\n";
        midiInputs.push_back(std::move(virtInput));
    } catch (...) {
        std::cout << "[RtMidi] Warning: Could not open virtual port.\n";
    }

    // ----------------------------------------------------
    // 3. Initialize PortAudio
    // ----------------------------------------------------
    PaError paErr = Pa_Initialize();
    if (paErr != paNoError) {
        std::cerr << "[PortAudio] Initialization error: " << Pa_GetErrorText(paErr) << "\n";
        return 1;
    }

    PaStreamParameters outputParams;
    outputParams.device = resolveAudioDevice(opts.audioDevice);
    if (outputParams.device == paNoDevice) {
        if (opts.audioDevice.empty()) {
            std::cerr << "[PortAudio] No default audio output device available!\n";
        } else {
            std::cerr << "[PortAudio] No stereo output device matches --audio-device '" << opts.audioDevice
                      << "' (see synth_sim --list-devices)\n";
        }
        Pa_Terminate();
        return 1;
    }

    const PaDeviceInfo* devInfo = Pa_GetDeviceInfo(outputParams.device);
    std::cout << "\n[PortAudio] Audio Device: " << (devInfo ? devInfo->name : "Unknown") << "\n";
    std::cout << "[PortAudio] Sample Rate : 44100 Hz | 16-bit Stereo | Buffer: " << opts.bufferFrames << " frames\n\n";

    outputParams.channelCount = 2; // Stereo
    outputParams.sampleFormat = paInt16;
    outputParams.suggestedLatency = devInfo ? devInfo->defaultLowOutputLatency : 0.010;
    outputParams.hostApiSpecificStreamInfo = nullptr;

    // SynthEngine::processBlock accepts any frame count (it renders in internal chunks), so the
    // buffer size only trades latency against dropout safety. Deferred events are applied at
    // the start of each render call, so note timing is quantised to the buffer size.
    PaStream* stream = nullptr;
    paErr = Pa_OpenStream(
        &stream,
        nullptr, // no input
        &outputParams,
        Synth::SynthConfig::DEFAULT_SAMPLE_RATE,
        static_cast<unsigned long>(opts.bufferFrames),
        paClipOff,
        paCallback,
        &ctx
    );

    if (paErr != paNoError) {
        std::cerr << "[PortAudio] OpenStream error: " << Pa_GetErrorText(paErr) << "\n";
        Pa_Terminate();
        return 1;
    }

    paErr = Pa_StartStream(stream);
    if (paErr != paNoError) {
        std::cerr << "[PortAudio] StartStream error: " << Pa_GetErrorText(paErr) << "\n";
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }

    const bool keyboardMode = opts.keyboard && !opts.testMode && isatty(STDIN_FILENO);

    std::cout << ">>> Synth simulator ready! <<<\n";
    printPresetList();
    std::cout << "Akai MPK Mini MK4: play keys & pads; turn SELECT or press < / > to change preset;\n"
                 "BANK A/B switches knobs K1..K8 between Bank A and Bank B.\n\n";
    SimKeyboard::Player player;
    if (keyboardMode) {
        std::cout << SimKeyboard::Player::helpText() << "[KEYS] " << player.statusLine() << "\n\n";
    } else if (!opts.testMode) {
        std::cout << "Computer-keyboard play mode off (" << (opts.keyboard ? "stdin is not a terminal" : "--no-keyboard")
                  << "). Quit with Ctrl+C.\n\n";
    }
    std::cout << std::flush;

    if (opts.testMode) {
        std::cout << "[Test Mode] Testing presets and bank toggle...\n";
        ctx.engine.loadPresetIndex(0);
        ctx.engine.toggleShift();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ctx.engine.loadPreset(Synth::SynthModel::DX_EPiano);
        std::cout << "[Test Mode] DX Classic E-Piano loaded! Mode=" << (ctx.engine.getEngineMode() == Synth::EngineMode::FM ? "FM" : "Subtractive") << "\n";
        ctx.engine.loadPreset(Synth::SynthModel::CasioPT10_Fantasy);
        std::cout << "[Test Mode] Casio PT-10 Fantasy loaded! Tone=" << ctx.engine.getCasioToneName() << "\n";
        ctx.engine.toggleShift();
        g_running = false;
    }

    // Main loop: MPK housekeeping, plus computer-keyboard input when enabled. Keyboard notes become
    // MIDI messages routed exactly like MPK input: UsbHostMidi -> SynthEngine::noteOn/noteOff, which
    // in deferred mode only queue events for the audio thread (no Voice state is touched here).
    std::optional<TerminalRawMode> rawMode;
    if (keyboardMode) rawMode.emplace();
    auto lastKeepalive = std::chrono::steady_clock::now();
    std::vector<SimKeyboard::MidiOut> keyMidi;

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(ctx.inputMutex);
            ctx.usbMidi.tick();

            // Send Screen Owner keepalive every 1.5 seconds to refresh controller lease timer
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastKeepalive).count() >= 1500) {
                ctx.usbMidi.sendKeepalive();
                lastKeepalive = now;
            }
        }

        if (!keyboardMode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // Wait up to 5 ms for keys so gate releases stay tight
        struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
        char keys[32];
        ssize_t nKeys = 0;
        if (poll(&pfd, 1, 5) > 0) nKeys = read(STDIN_FILENO, keys, sizeof(keys));

        for (ssize_t k = 0; k < nKeys && g_running; ++k) {
            keyMidi.clear();
            const SimKeyboard::Command cmd = player.handleKey(keys[k], nowMs(), keyMidi);
            routeKeyboardMidi(ctx, keyMidi);

            using SimKeyboard::Command;
            switch (cmd) {
                case Command::PresetPrev:
                case Command::PresetNext: {
                    std::lock_guard<std::mutex> lock(ctx.inputMutex);
                    ctx.usbMidi.stepPreset(cmd == Command::PresetNext ? 1 : -1);
                    break;
                }
                case Command::ToggleBank: {
                    std::lock_guard<std::mutex> lock(ctx.inputMutex);
                    ctx.usbMidi.setBankBLatched(!ctx.usbMidi.isBankBLatched());
                    break;
                }
                case Command::OctaveDown:
                case Command::OctaveUp:
                case Command::Velocity:
                case Command::GateShorter:
                case Command::GateLonger:
                case Command::ToggleHold:
                    std::cout << "\r[KEYS] " << player.statusLine() << "    \n" << std::flush;
                    break;
                case Command::Panic:
                    std::cout << "\r[KEYS] Panic: all sound off    \n" << std::flush;
                    break;
                case Command::Help:
                    std::cout << "\n" << SimKeyboard::Player::helpText() << "[KEYS] " << player.statusLine() << "\n\n" << std::flush;
                    break;
                case Command::Quit:
                    g_running = false;
                    break;
                default:
                    break;
            }
        }

        keyMidi.clear();
        player.tick(nowMs(), keyMidi);
        routeKeyboardMidi(ctx, keyMidi);
    }

    rawMode.reset();
    std::cout << "\nShutting down audio and MIDI streams...\n";
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();

    std::cout << "Done.\n";
    return 0;
}
