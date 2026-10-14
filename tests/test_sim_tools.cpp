#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "sim_util.h"
#include "smf_reader.h"
#include "sim_keyboard.h"

using SimKeyboard::Command;
using SimKeyboard::MidiOut;
using SimKeyboard::Player;

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

void test_note_names() {
    std::cout << "[Test] Running note name parsing test..." << std::endl;
    int n = 0;
    assert(SimUtil::parseNoteName("C4", n) && n == 60);
    assert(SimUtil::parseNoteName("c4", n) && n == 60);
    assert(SimUtil::parseNoteName("A4", n) && n == 69);
    assert(SimUtil::parseNoteName("F#3", n) && n == 54);
    assert(SimUtil::parseNoteName("Bb2", n) && n == 46);
    assert(SimUtil::parseNoteName("C-1", n) && n == 0);
    assert(SimUtil::parseNoteName("G9", n) && n == 127);
    assert(SimUtil::parseNoteName("60", n) && n == 60);
    assert(!SimUtil::parseNoteName("G#9", n));   // 128
    assert(!SimUtil::parseNoteName("H4", n));
    assert(!SimUtil::parseNoteName("C", n));
    assert(!SimUtil::parseNoteName("C4x", n));
    assert(!SimUtil::parseNoteName("128", n));
    assert(SimUtil::noteName(60) == "C4");
    assert(SimUtil::noteName(61) == "C#4");
    assert(SimUtil::noteName(0) == "C-1");
    std::cout << "  -> Note name tests PASSED!" << std::endl;
}

void test_note_sequence() {
    std::cout << "[Test] Running --notes sequence spec test..." << std::endl;
    std::vector<SimUtil::TimedMidi> ev;
    double end = 0.0;
    std::string err;

    assert(SimUtil::parseNoteSequence("C4,E4,G4", 2.0, 100, ev, end, err));
    assert(ev.size() == 6 && near(end, 2.0));
    assert(ev[0].status == 0x90 && ev[0].data1 == 60 && ev[0].data2 == 100 && near(ev[0].timeSec, 0.0));
    assert(ev[1].status == 0x80 && ev[1].data1 == 60 && near(ev[1].timeSec, 2.0));

    ev.clear();
    assert(SimUtil::parseNoteSequence("  C3  r:0.25 E3:1.5 ", 0.5, 90, ev, end, err));
    assert(ev.size() == 4 && near(end, 0.5 + 0.25 + 1.5));
    assert(ev[2].data1 == 52 && near(ev[2].timeSec, 0.75) && near(ev[3].timeSec, 2.25));

    ev.clear();
    assert(!SimUtil::parseNoteSequence("C4,,E4", 1.0, 100, ev, end, err));
    assert(!SimUtil::parseNoteSequence("C4:0", 1.0, 100, ev, end, err));
    assert(!SimUtil::parseNoteSequence("   ", 1.0, 100, ev, end, err));
    std::cout << "  -> Note sequence tests PASSED!" << std::endl;
}

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
    return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | static_cast<uint32_t>(b[at + 3]) << 24;
}

void test_wav_writer() {
    std::cout << "[Test] Running WAV writer test..." << std::endl;
    const std::string path = "sim_tools_test_tmp.wav";
    const std::vector<float> samples = {0.0f, 1.0f, -1.0f, 0.5f, 2.0f, -2.0f};

    assert(SimUtil::writeWav(path, samples, 2, 44100, false));
    auto b = readFile(path);
    assert(b.size() == 44 + samples.size() * 2);
    assert(std::memcmp(b.data(), "RIFF", 4) == 0 && le32(b, 4) == b.size() - 8);
    assert(std::memcmp(b.data() + 8, "WAVEfmt ", 8) == 0);
    assert(b[20] == 1 && b[22] == 2 && le32(b, 24) == 44100 && b[34] == 16);
    assert(std::memcmp(b.data() + 36, "data", 4) == 0 && le32(b, 40) == samples.size() * 2);
    auto s16 = [&](size_t i) { return static_cast<int16_t>(b[44 + 2 * i] | b[45 + 2 * i] << 8); };
    assert(s16(0) == 0 && s16(1) == 32767 && s16(2) == -32767 && s16(3) == 16384);
    assert(s16(4) == 32767 && s16(5) == -32767); // clamped

    assert(SimUtil::writeWav(path, samples, 2, 44100, true));
    b = readFile(path);
    assert(b.size() == 58 + samples.size() * 4 && le32(b, 4) == b.size() - 8);
    assert(b[20] == 3 && b[34] == 32 && le32(b, 16) == 18);
    assert(std::memcmp(b.data() + 38, "fact", 4) == 0 && le32(b, 46) == 3);
    assert(std::memcmp(b.data() + 50, "data", 4) == 0);
    float f = 0.0f;
    std::memcpy(&f, b.data() + 58 + 4 * 4, 4);
    assert(f == 2.0f); // float output is not clamped
    std::remove(path.c_str());
    std::cout << "  -> WAV writer tests PASSED!" << std::endl;
}

void test_loudness_meter() {
    std::cout << "[Test] Running BS.1770 loudness meter test..." << std::endl;
    // A full-scale 997 Hz sine on both channels reads 0 LUFS; one channel only reads -3.01 LUFS
    const uint32_t rate = 44100;
    std::vector<float> both(rate * 2), left(rate * 2);
    for (size_t i = 0; i < rate; ++i) {
        const float s = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 997.0 * i / rate));
        both[2 * i] = both[2 * i + 1] = s;
        left[2 * i] = s;
    }
    assert(std::fabs(SimUtil::maxMomentaryLoudness(both, rate)) < 0.1);
    assert(std::fabs(SimUtil::maxMomentaryLoudness(left, rate) + 3.01) < 0.1);

    // Halving the amplitude is -6.02 LU; the loudest 400 ms wins over a quieter remainder
    for (size_t i = rate / 2; i < rate; ++i) both[2 * i] = both[2 * i + 1] = both[2 * i] * 0.5f;
    assert(std::fabs(SimUtil::maxMomentaryLoudness(both, rate)) < 0.1);
    for (size_t i = 0; i < rate / 2; ++i) both[2 * i] = both[2 * i + 1] = both[2 * i] * 0.5f;
    assert(std::fabs(SimUtil::maxMomentaryLoudness(both, rate) + 6.02) < 0.1);

    assert(SimUtil::maxMomentaryLoudness(std::vector<float>(rate), rate) == -999.0); // silence
    assert(SimUtil::maxMomentaryLoudness(std::vector<float>(100, 0.5f), rate) == -999.0); // too short
    std::cout << "  -> Loudness meter tests PASSED!" << std::endl;
}

void test_smf_reader() {
    std::cout << "[Test] Running Standard MIDI File reader test..." << std::endl;
    // Format 1, 480 PPQ. Track 1: tempo 120 BPM, then 60 BPM at tick 480.
    // Track 2: note on C4 at 0, running-status note on E4 at 0, SysEx at 240 (skipped),
    // note off C4 (vel-0 note on) at 480, note off E4 at 960, program change (skipped).
    std::vector<uint8_t> f = {
        'M','T','h','d', 0,0,0,6, 0,1, 0,2, 0x01,0xE0,
        'M','T','r','k', 0,0,0,19,
            0x00, 0xFF,0x51,0x03, 0x07,0xA1,0x20,
            0x83,0x60, 0xFF,0x51,0x03, 0x0F,0x42,0x40,
            0x00, 0xFF,0x2F,0x00,
        'M','T','r','k', 0,0,0,30,
            0x00, 0x90,60,100,
            0x00, 64,90,
            0x81,0x70, 0xF0,0x02,0x7E,0xF7,
            0x81,0x70, 0x90,60,0,
            0x83,0x60, 0x80,64,0,
            0x00, 0xC0,5,
            0x00, 0xFF,0x2F,0x00,
    };
    std::vector<SimUtil::TimedMidi> ev;
    std::string err;
    assert(SimUtil::parseSmf(f, ev, err));
    assert(ev.size() == 4);
    assert(ev[0].status == 0x90 && ev[0].data1 == 60 && near(ev[0].timeSec, 0.0));
    assert(ev[1].status == 0x90 && ev[1].data1 == 64 && ev[1].data2 == 90);
    assert(ev[2].status == 0x90 && ev[2].data2 == 0 && near(ev[2].timeSec, 0.5));   // 480 ticks @ 120 BPM
    assert(ev[3].status == 0x80 && ev[3].data1 == 64 && near(ev[3].timeSec, 1.5)); // + 480 ticks @ 60 BPM

    std::vector<uint8_t> bad = {'M','T','h','d', 0,0,0,6, 0,2, 0,1, 0x01,0xE0};
    assert(!SimUtil::parseSmf(bad, ev, err));
    bad = {'R','I','F','F'};
    assert(!SimUtil::parseSmf(bad, ev, err));
    std::cout << "  -> SMF reader tests PASSED!" << std::endl;
}

void test_key_map() {
    std::cout << "[Test] Running computer keyboard key map test..." << std::endl;
    const char* piano = "awsedftgyhujkolp;";
    for (int i = 0; piano[i]; ++i) {
        auto kb = SimKeyboard::mapKey(piano[i]);
        assert(kb.cmd == Command::Note && kb.value == i);
    }
    assert(SimKeyboard::mapKey('A').cmd == Command::Note);        // caps lock
    assert(SimKeyboard::mapKey('k').value == 12);                 // C one octave up
    assert(SimKeyboard::mapKey('z').cmd == Command::OctaveDown);
    assert(SimKeyboard::mapKey('x').cmd == Command::OctaveUp);
    assert(SimKeyboard::mapKey(',').cmd == Command::PresetPrev);
    assert(SimKeyboard::mapKey('[').cmd == Command::PresetPrev);
    assert(SimKeyboard::mapKey('.').cmd == Command::PresetNext);
    assert(SimKeyboard::mapKey(']').cmd == Command::PresetNext);
    assert(SimKeyboard::mapKey('1').cmd == Command::Velocity && SimKeyboard::mapKey('1').value == 14);
    assert(SimKeyboard::mapKey('9').value == 127);
    assert(SimKeyboard::mapKey(' ').cmd == Command::Panic);
    assert(SimKeyboard::mapKey('m').cmd == Command::ToggleHold);
    assert(SimKeyboard::mapKey('b').cmd == Command::ToggleBank);
    assert(SimKeyboard::mapKey('-').cmd == Command::GateShorter);
    assert(SimKeyboard::mapKey('=').cmd == Command::GateLonger);
    assert(SimKeyboard::mapKey('?').cmd == Command::Help);
    assert(SimKeyboard::mapKey('q').cmd == Command::Quit);
    assert(SimKeyboard::mapKey('0').cmd == Command::None);
    assert(SimKeyboard::mapKey('\x1b').cmd == Command::None);
    std::cout << "  -> Key map tests PASSED!" << std::endl;
}

void test_gate_mode() {
    std::cout << "[Test] Running keyboard Gate mode test..." << std::endl;
    Player p;
    std::vector<MidiOut> out;

    assert(p.handleKey('a', 1000, out) == Command::Note);
    assert(out.size() == 1 && out[0].status == 0x90 && out[0].data1 == 60 && out[0].data2 == 100);
    out.clear();

    // Released after the gate, not before
    p.tick(1000 + Player::kDefaultGateMs - 1, out);
    assert(out.empty());
    p.tick(1000 + Player::kDefaultGateMs, out);
    assert(out.size() == 1 && out[0].status == 0x80 && out[0].data1 == 60);
    assert(p.soundingCount() == 0);
    out.clear();

    // Held key: autorepeat extends the note without retriggering
    p.handleKey('d', 2000, out);
    for (uint64_t t = 2030; t <= 3000; t += 30) p.handleKey('d', t, out);
    assert(out.size() == 1);
    p.tick(2990 + Player::kDefaultGateMs - 1, out);
    assert(out.size() == 1);
    p.tick(2990 + Player::kDefaultGateMs, out);
    assert(out.size() == 2 && out[1].status == 0x80 && out[1].data1 == 64);
    out.clear();

    // A deliberate re-press while still sounding retriggers (off, then on)
    p.handleKey('g', 5000, out);
    p.handleKey('g', 5200, out);
    assert(out.size() == 3 && out[1].status == 0x80 && out[2].status == 0x90 && out[2].data1 == 67);
    out.clear();

    // Octave, velocity and gate controls
    p.handleKey('x', 6000, out);
    p.handleKey('9', 6000, out);
    p.handleKey('=', 6000, out);
    assert(p.octave() == 5 && p.velocity() == 127 && p.gateMs() == Player::kDefaultGateMs + 100);
    p.handleKey('a', 6000, out);
    assert(out.back().status == 0x90 && out.back().data1 == 72 && out.back().data2 == 127);
    for (int i = 0; i < 20; ++i) p.handleKey('z', 6000, out);
    assert(p.octave() == Player::kMinOctave);
    for (int i = 0; i < 40; ++i) p.handleKey('-', 6000, out);
    assert(p.gateMs() == Player::kMinGateMs);
    for (int i = 0; i < 20; ++i) p.handleKey('x', 6000, out);
    assert(p.octave() == Player::kMaxOctave);
    out.clear();
    p.handleKey(';', 7000, out);
    assert(out.size() == 1 && out[0].data1 == 124);

    // Panic: CC 120 and nothing left to release
    out.clear();
    assert(p.handleKey(' ', 7010, out) == Command::Panic);
    assert(out.size() == 1 && out[0].status == 0xB0 && out[0].data1 == 120);
    out.clear();
    p.tick(100000, out);
    assert(out.empty());
    std::cout << "  -> Gate mode tests PASSED!" << std::endl;
}

void test_hold_mode() {
    std::cout << "[Test] Running keyboard Hold mode test..." << std::endl;
    Player p;
    std::vector<MidiOut> out;

    p.handleKey('a', 0, out);               // Gate-mode note...
    assert(p.handleKey('m', 10, out) == Command::ToggleHold);
    assert(p.holdMode());
    assert(out.size() == 2 && out[1].status == 0x80); // ...released on the mode switch
    out.clear();

    // Build a chord, notes never auto-release
    p.handleKey('a', 1000, out);
    p.handleKey('d', 1200, out);
    p.handleKey('g', 1400, out);
    p.handleKey('g', 1430, out);            // autorepeat: ignored
    assert(out.size() == 3 && p.soundingCount() == 3);
    p.tick(1000000, out);
    assert(out.size() == 3);

    // Second press releases one note
    p.handleKey('d', 2000, out);
    assert(out.size() == 4 && out[3].status == 0x80 && out[3].data1 == 64 && p.soundingCount() == 2);
    p.handleKey('d', 2030, out);            // autorepeat of the release: ignored
    assert(out.size() == 4);

    // Quit releases everything that is still held
    out.clear();
    assert(p.handleKey('q', 3000, out) == Command::Quit);
    assert(out.size() == 2 && out[0].status == 0x80 && out[1].status == 0x80);
    assert(p.soundingCount() == 0);
    std::cout << "  -> Hold mode tests PASSED!" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "  Running Simulator / Renderer Helper Unit Tests  " << std::endl;
    std::cout << "==================================================" << std::endl;

    test_note_names();
    test_note_sequence();
    test_wav_writer();
    test_loudness_meter();
    test_smf_reader();
    test_key_map();
    test_gate_mode();
    test_hold_mode();

    std::cout << "All simulator helper tests PASSED!" << std::endl;
    return 0;
}
