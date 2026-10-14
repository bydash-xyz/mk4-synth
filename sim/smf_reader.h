#pragma once

// Minimal Standard MIDI File reader for synth_render: format 0/1, PPQ time division, tempo map.
// Keeps Note On/Off, Control Change and Pitch Bend from every track and channel; everything else
// (program changes, aftertouch, SysEx, other meta events) is skipped. Header-only, no dependencies.

#include "sim_util.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace SimUtil {

inline bool parseSmf(const std::vector<uint8_t>& bytes, std::vector<TimedMidi>& out, std::string& err) {
    size_t pos = 0;
    auto need = [&](size_t n) { return pos + n <= bytes.size(); };
    auto be16 = [&](size_t at) { return static_cast<uint32_t>(bytes[at] << 8 | bytes[at + 1]); };
    auto be32 = [&](size_t at) { return be16(at) << 16 | be16(at + 2); };

    if (!need(14) || std::string(bytes.begin(), bytes.begin() + 4) != "MThd" || be32(4) < 6) {
        err = "not a Standard MIDI File (missing MThd header)";
        return false;
    }
    const uint32_t format   = be16(8);
    const uint32_t division = be16(12);
    if (format > 1) { err = "SMF format 2 is not supported"; return false; }
    if (division & 0x8000) { err = "SMPTE time division is not supported"; return false; }
    if (division == 0) { err = "invalid time division"; return false; }
    pos = 8 + be32(4);

    struct RawEvent { uint32_t tick; uint32_t seq; uint32_t tempo; uint8_t status, d1, d2; };
    std::vector<RawEvent> events; // tempo != 0 marks a tempo change
    uint32_t seq = 0;

    while (need(8)) {
        const std::string id(bytes.begin() + pos, bytes.begin() + pos + 4);
        const uint32_t len = be32(pos + 4);
        pos += 8;
        if (!need(len)) { err = "truncated chunk '" + id + "'"; return false; }
        const size_t end = pos + len;
        if (id != "MTrk") { pos = end; continue; }

        uint32_t tick = 0;
        uint8_t running = 0;
        auto readVlq = [&](uint32_t& v) {
            v = 0;
            for (int n = 0; n < 4; ++n) {
                if (pos >= end) return false;
                const uint8_t b = bytes[pos++];
                v = (v << 7) | (b & 0x7F);
                if (!(b & 0x80)) return true;
            }
            return false;
        };

        while (pos < end) {
            uint32_t delta = 0;
            if (!readVlq(delta)) { err = "bad delta time"; return false; }
            tick += delta;
            if (pos >= end) { err = "truncated track"; return false; }

            uint8_t status = bytes[pos];
            if (status & 0x80) { ++pos; } else if (running) { status = running; }
            else { err = "running status without a previous status byte"; return false; }

            if (status == 0xFF) {                         // meta event
                if (pos >= end) { err = "truncated meta event"; return false; }
                const uint8_t type = bytes[pos++];
                uint32_t mlen = 0;
                if (!readVlq(mlen) || pos + mlen > end) { err = "truncated meta event"; return false; }
                if (type == 0x51 && mlen == 3) {
                    const uint32_t us = bytes[pos] << 16 | bytes[pos + 1] << 8 | bytes[pos + 2];
                    if (us > 0) events.push_back({tick, seq++, us, 0, 0, 0});
                }
                pos += mlen;
                if (type == 0x2F) break;                  // end of track
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {       // SysEx: skip
                uint32_t slen = 0;
                if (!readVlq(slen) || pos + slen > end) { err = "truncated SysEx event"; return false; }
                pos += slen;
                continue;
            }
            if (status >= 0xF0) { err = "unexpected system message in track"; return false; }

            running = status;
            const uint8_t type = status & 0xF0;
            const size_t dataBytes = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            if (pos + dataBytes > end) { err = "truncated channel message"; return false; }
            const uint8_t d1 = bytes[pos];
            const uint8_t d2 = dataBytes == 2 ? bytes[pos + 1] : 0;
            pos += dataBytes;
            if (type == 0x80 || type == 0x90 || type == 0xB0 || type == 0xE0) {
                events.push_back({tick, seq++, 0, status, d1, d2});
            }
        }
        pos = end;
    }

    // Merge tracks: by tick, tempo changes first, then file order
    std::sort(events.begin(), events.end(), [](const RawEvent& a, const RawEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if ((a.tempo != 0) != (b.tempo != 0)) return a.tempo != 0;
        return a.seq < b.seq;
    });

    double sec = 0.0;
    uint32_t lastTick = 0;
    uint32_t usPerQuarter = 500000; // 120 BPM default
    for (const auto& e : events) {
        sec += static_cast<double>(e.tick - lastTick) * usPerQuarter / (1.0e6 * division);
        lastTick = e.tick;
        if (e.tempo) usPerQuarter = e.tempo;
        else out.push_back({sec, e.status, e.d1, e.d2});
    }
    return true;
}

inline bool readSmfFile(const std::string& path, std::vector<TimedMidi>& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseSmf(bytes, out, err);
}

} // namespace SimUtil
