#include "SynthEngine.h"
#include "MidiParser.h"
#include "SysExProtocol.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include <cmath>

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "--- Running SysEx Protocol & Storage Unit Tests ---" << std::endl;
    std::cout << "==================================================" << std::endl;

    // -------------------------------------------------------------
    // Test 1: 7-in-8 Bit Packing and Unpacking
    // -------------------------------------------------------------
    std::cout << "\nTest 1: 7-in-8 Bit Packing & Unpacking round-trip..." << std::endl;
    {
        uint8_t rawOriginal[Synth::RAW_PATCH_SIZE];
        for (size_t i = 0; i < Synth::RAW_PATCH_SIZE; ++i) {
            // Fill with diverse byte values, including high bits set (>= 0x80)
            rawOriginal[i] = static_cast<uint8_t>((i * 37 + 0x85) & 0xFF);
        }

        uint8_t packedMidi[128];
        size_t packedLen = Synth::SysExProtocol::pack7Bit(
            rawOriginal, Synth::RAW_PATCH_SIZE, packedMidi, sizeof(packedMidi)
        );

        assert(packedLen == Synth::PACKED_PATCH_SIZE);
        assert(packedLen == 55);

        // Verify all packed bytes are 7-bit safe (MSB = 0)
        for (size_t i = 0; i < packedLen; ++i) {
            assert((packedMidi[i] & 0x80) == 0);
        }

        uint8_t rawRestored[Synth::RAW_PATCH_SIZE];
        size_t unpackedLen = Synth::SysExProtocol::unpack7Bit(
            packedMidi, packedLen, rawRestored, sizeof(rawRestored)
        );

        assert(unpackedLen == Synth::RAW_PATCH_SIZE);
        assert(std::memcmp(rawOriginal, rawRestored, Synth::RAW_PATCH_SIZE) == 0);

        std::cout << "  Passed (55-byte 7-bit MIDI safe encoding & 100% bit preservation verified)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 2: Checksum Verification & Rejection
    // -------------------------------------------------------------
    std::cout << "\nTest 2: XOR Checksum Calculation & Tamper Rejection..." << std::endl;
    {
        Synth::RawPatchData patch{};
        std::strncpy(patch.name, "TEST PATCH", sizeof(patch.name) - 1);
        patch.cutoffHz = 32000;
        patch.resonanceQ = 15000;

        uint8_t buffer[128];
        size_t encodedLen = Synth::SysExProtocol::encodePatchDump(
            Synth::USER_PRESET_SLOT, patch, buffer, sizeof(buffer)
        );
        assert(encodedLen == Synth::FULL_DUMP_PACKET_SIZE);
        assert(encodedLen == 62);

        // Verify valid packet decodes successfully
        Synth::DecodedSysEx decoded = Synth::SysExProtocol::decodeSysEx(buffer, encodedLen);
        assert(decoded.isValid);
        assert(decoded.type == Synth::SysExCommand::PatchDump);
        assert(decoded.slot == Synth::USER_PRESET_SLOT);
        assert(std::strcmp(decoded.patchData.name, "TEST PATCH") == 0);
        assert(decoded.patchData.cutoffHz == 32000);

        // Corrupt one data byte in the payload
        buffer[10] ^= 0x01;
        Synth::DecodedSysEx corruptedData = Synth::SysExProtocol::decodeSysEx(buffer, encodedLen);
        assert(!corruptedData.isValid);
        assert(corruptedData.errorStatus == Synth::HandshakeStatus::NackChecksumErr);

        // Restore data byte and corrupt checksum byte directly
        buffer[10] ^= 0x01;
        buffer[5 + Synth::PACKED_PATCH_SIZE] ^= 0x04;
        Synth::DecodedSysEx corruptedChecksum = Synth::SysExProtocol::decodeSysEx(buffer, encodedLen);
        assert(!corruptedChecksum.isValid);
        assert(corruptedChecksum.errorStatus == Synth::HandshakeStatus::NackChecksumErr);

        std::cout << "  Passed (Checksum verifies valid packets and rejects bit flips)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 3: Fast Single Parameter Update (Command 0x01)
    // -------------------------------------------------------------
    std::cout << "\nTest 3: Fast Single Parameter Updates & Live DSP..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);
        Synth::MidiParser parser;

        // 3a. Update Cutoff to high value (14-bit max = 16383 -> ~20000 Hz)
        uint8_t sysexCutoffMax[10];
        size_t len = Synth::SysExProtocol::encodeFastParamUpdate(
            Synth::ParamId::Cutoff, 16383, 0, sysexCutoffMax, sizeof(sysexCutoffMax)
        );
        parser.parseBuffer(sysexCutoffMax, len, engine);
        assert(engine.getCutoffHz() > 19000.0f);

        // 3b. Update Cutoff to low value (14-bit min = 0 -> ~20 Hz)
        uint8_t sysexCutoffMin[10];
        len = Synth::SysExProtocol::encodeFastParamUpdate(
            Synth::ParamId::Cutoff, 0, 0, sysexCutoffMin, sizeof(sysexCutoffMin)
        );
        parser.parseBuffer(sysexCutoffMin, len, engine);
        assert(engine.getCutoffHz() < 30.0f);

        // 3c. Update Resonance (Q)
        uint8_t sysexRes[10];
        len = Synth::SysExProtocol::encodeFastParamUpdate(
            Synth::ParamId::Resonance, 8192, 0, sysexRes, sizeof(sysexRes)
        );
        parser.parseBuffer(sysexRes, len, engine);
        assert(engine.getResonanceQ() > 10.0f);

        // 3d. Update Waveform to Square (val 1)
        uint8_t sysexWave[10];
        len = Synth::SysExProtocol::encodeFastParamUpdate(
            Synth::ParamId::Waveform, 1, 0, sysexWave, sizeof(sysexWave)
        );
        parser.parseBuffer(sysexWave, len, engine);
        assert(engine.getWaveform() == Synth::Waveform::Square);

        // 3e. Update SubOsc, Detune, Drive
        uint8_t sysexSub[10];
        len = Synth::SysExProtocol::encodeFastParamUpdate(
            Synth::ParamId::SubOscLevel, 16383, 0, sysexSub, sizeof(sysexSub)
        );
        parser.parseBuffer(sysexSub, len, engine);
        assert(engine.getSubOscLevel() >= 0.99f);

        uint8_t sysexDrive[10];
        len = Synth::SysExProtocol::encodeFastParamUpdate(
            Synth::ParamId::Drive, 16383, 0, sysexDrive, sizeof(sysexDrive)
        );
        parser.parseBuffer(sysexDrive, len, engine);
        assert(engine.getDrive() >= 3.4f);

        // 3f. Update Voice Mode to Monophonic and Glide Time
        uint8_t sysexMode[10];
        len = Synth::SysExProtocol::encodeFastParamUpdate(
            Synth::ParamId::VoiceMode, 1, 0, sysexMode, sizeof(sysexMode)
        );
        parser.parseBuffer(sysexMode, len, engine);
        assert(engine.getVoiceMode() == Synth::VoiceMode::Monophonic);

        uint8_t sysexGlide[10];
        len = Synth::SysExProtocol::encodeFastParamUpdate(
            Synth::ParamId::GlideTime, 8192, 0, sysexGlide, sizeof(sysexGlide)
        );
        parser.parseBuffer(sysexGlide, len, engine);
        assert(engine.getGlideTimeMs() > 400.0f && engine.getGlideTimeMs() < 600.0f);

        // Verify audio rendering with modified parameters
        engine.noteOn(60, 100);
        int16_t outPcm[256];
        engine.processBlock(outPcm, 128);
        bool hasSignal = false;
        for (int i = 0; i < 256; ++i) {
            if (outPcm[i] != 0) hasSignal = true;
        }
        assert(hasSignal);

        std::cout << "  Passed (14-bit resolution scaling and audio rendering verified)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 4: Full Patch Dump to Live Edit Buffer (Slot 0)
    // -------------------------------------------------------------
    std::cout << "\nTest 4: Full Patch Dump (Slot 0 Live Edit Buffer & Handshake)..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);
        Synth::MidiParser parser;

        Synth::RawPatchData dumpData{};
        std::strncpy(dumpData.name, "HYPER SAW", sizeof(dumpData.name) - 1);
        dumpData.name[sizeof(dumpData.name) - 1] = '\0';
        dumpData.waveform = static_cast<uint8_t>(Synth::Waveform::Sawtooth);
        dumpData.cutoffHz = 58000;
        dumpData.resonanceQ = 20000;
        dumpData.subOscLevel = 30000;
        dumpData.detuneCents = 40000;
        dumpData.drive = 20000;
        dumpData.flags = 0x01; // Chorus enabled

        uint8_t dumpPacket[128];
        size_t dumpLen = Synth::SysExProtocol::encodePatchDump(0, dumpData, dumpPacket, sizeof(dumpPacket));

        parser.parseBuffer(dumpPacket, dumpLen, engine);

        // Verify engine live parameters updated
        assert(engine.getWaveform() == Synth::Waveform::Sawtooth);
        assert(engine.isChorusEnabled());
        assert(engine.getCutoffHz() > 5000.0f);

        // Verify Handshake response (ACK)
        const auto& resp = engine.getLastSysExResponse();
        assert(!resp.empty());
        Synth::DecodedSysEx handshake = Synth::SysExProtocol::decodeSysEx(resp.data(), resp.size());
        assert(handshake.isValid);
        assert(handshake.type == Synth::SysExCommand::Handshake);
        assert(handshake.refCommand == Synth::SysExCommand::PatchDump);
        assert(handshake.handshakeStatus == Synth::HandshakeStatus::AckSuccess);

        // Attempting to overwrite a read-only factory preset (#1) must yield NACK
        dumpLen = Synth::SysExProtocol::encodePatchDump(1, dumpData, dumpPacket, sizeof(dumpPacket));
        parser.parseBuffer(dumpPacket, dumpLen, engine);
        const auto& respNack = engine.getLastSysExResponse();
        Synth::DecodedSysEx nack = Synth::SysExProtocol::decodeSysEx(respNack.data(), respNack.size());
        assert(nack.isValid);
        assert(nack.handshakeStatus == Synth::HandshakeStatus::NackInvalidSlot);

        std::cout << "  Passed (Live edit buffer dump accepted with ACK; factory slot write rejected with NACK)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 5: Dedicated User Preset Slot #24 (Save & NVS Persistence)
    // -------------------------------------------------------------
    std::cout << "\nTest 5: User Preset Slot #24 Save, Restore & HAL Storage Persistence..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);
        Synth::MidiParser parser;

        // Set distinctive live engine values
        engine.setCutoffHz(7777.0f);
        engine.setResonanceQ(5.5f);
        engine.setWaveform(Synth::Waveform::Triangle);
        engine.setSubOscLevel(0.85f);
        engine.setDrive(2.2f);
        engine.setChorusEnabled(true);
        engine.setChorusMix(0.65f);

        // Send Command 0x03 (Save Active Patch to User Slot #7)
        uint8_t savePacket[16];
        size_t saveLen = Synth::SysExProtocol::encodeSaveUserSlot(
            Synth::USER_PRESET_SLOT, savePacket, sizeof(savePacket)
        );
        parser.parseBuffer(savePacket, saveLen, engine);

        // Check ACK response
        const auto& resp = engine.getLastSysExResponse();
        Synth::DecodedSysEx ack = Synth::SysExProtocol::decodeSysEx(resp.data(), resp.size());
        assert(ack.isValid);
        assert(ack.type == Synth::SysExCommand::Handshake);
        assert(ack.refCommand == Synth::SysExCommand::SaveUserSlot);
        assert(ack.handshakeStatus == Synth::HandshakeStatus::AckSuccess);

        // Switch to Minimoog (Preset 1) to alter current active parameters
        engine.loadPreset(Synth::SynthModel::Minimoog);
        assert(engine.getWaveform() == Synth::Waveform::Sawtooth);

        // Switch to User Preset (Slot #7)
        engine.loadPreset(Synth::SynthModel::User);
        assert(engine.getWaveform() == Synth::Waveform::Triangle);
        assert(std::abs(engine.getCutoffHz() - 7777.0f) < 200.0f);
        assert(std::abs(engine.getSubOscLevel() - 0.85f) < 0.05f);
        assert(engine.isChorusEnabled());

        // Test HAL Storage Callbacks (e.g. simulated Flash/EEPROM): v2 bodies of variable length
        std::vector<uint8_t> mockHalStorage;
        bool mockWriteCalled = false;
        bool mockReadCalled = false;

        auto readMock = [&mockHalStorage, &mockReadCalled](uint8_t slot, uint8_t* outData, size_t maxSize) -> size_t {
            mockReadCalled = true;
            if (slot != Synth::USER_PRESET_SLOT || mockHalStorage.empty() || mockHalStorage.size() > maxSize) return 0;
            std::memcpy(outData, mockHalStorage.data(), mockHalStorage.size());
            return mockHalStorage.size();
        };
        engine.setStorageCallbacks(
            [&mockHalStorage, &mockWriteCalled](uint8_t slot, const uint8_t* data, size_t size) {
                assert(slot == Synth::USER_PRESET_SLOT);
                assert(size > 0 && size <= Synth::MAX_PATCH_BODY_SIZE);
                mockWriteCalled = true;
                mockHalStorage.assign(data, data + size);
                return true;
            },
            readMock
        );

        // Save again through HAL hook
        bool saved = engine.saveUserPresetToStorage();
        assert(saved && mockWriteCalled);
        assert(Synth::SysExProtocol::isValidPatchBody(mockHalStorage.data(), mockHalStorage.size()));

        // Create a completely new synth engine and wire the HAL read callback
        Synth::SynthEngine freshEngine;
        freshEngine.setStorageCallbacks(nullptr, readMock);
        freshEngine.init(44100.0f);

        // Verify fresh engine restored custom user preset from HAL storage
        freshEngine.loadPreset(Synth::SynthModel::User);
        assert(mockReadCalled);
        assert(freshEngine.getWaveform() == Synth::Waveform::Triangle);
        assert(std::abs(freshEngine.getCutoffHz() - 7777.0f) < 200.0f);

        std::cout << "  Passed (User Slot #24 saved in memory & persisted across restarts via HAL callbacks)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 6: Request Patch Dump (Command 0x04)
    // -------------------------------------------------------------
    std::cout << "\nTest 6: Request Patch Dump from Slot (Command 0x04)..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);
        Synth::MidiParser parser;

        // Requests a slot and decodes the PatchDumpV2 reply on top of the default user patch
        char name[Synth::PATCH_NAME_MAX + 1];
        auto request = [&](uint8_t slot, Synth::Preset& out) {
            uint8_t reqPacket[16];
            size_t reqLen = Synth::SysExProtocol::encodeRequestPatchDump(slot, reqPacket, sizeof(reqPacket));
            parser.parseBuffer(reqPacket, reqLen, engine);
            const auto& resp = engine.getLastSysExResponse();
            Synth::DecodedSysEx dump = Synth::SysExProtocol::decodeSysEx(resp.data(), resp.size());
            out = Synth::getPresets()[Synth::FACTORY_PRESET_COUNT];
            if (dump.isValid && dump.type == Synth::SysExCommand::PatchDumpV2) {
                assert(dump.slot == slot);
                assert(Synth::SysExProtocol::decodePatchBody(dump.patchBody.data(), dump.patchBody.size(), out, name));
            }
            return dump;
        };

        // Factory slots 1..23: name, voicing (formerly the v1 model byte) and engine survive the dump
        struct Expect { uint8_t slot; const char* name; Synth::SynthModel model; Synth::EngineMode mode; };
        const Expect factory[] = {
            {1,  "JUNO-60",     Synth::SynthModel::Juno60,            Synth::EngineMode::Subtractive},
            {2,  "MINIMOOG",    Synth::SynthModel::Minimoog,          Synth::EngineMode::Subtractive},
            {7,  "PT-10 PIANO", Synth::SynthModel::CasioPT10_Piano,   Synth::EngineMode::Subtractive},
            {8,  "PT-10 FNTSY", Synth::SynthModel::CasioPT10_Fantasy, Synth::EngineMode::Subtractive},
            {9,  "PT-10 VIOLN", Synth::SynthModel::CasioPT10_Violin,  Synth::EngineMode::Subtractive},
            {10, "PT-10 FLUTE", Synth::SynthModel::CasioPT10_Flute,   Synth::EngineMode::Subtractive},
            {11, "DX E-PIANO",  Synth::SynthModel::DX_EPiano,         Synth::EngineMode::FM},
            {12, "FM TINES",    Synth::SynthModel::FM_CrystalTines,   Synth::EngineMode::FM},
            {13, "RHODES",      Synth::SynthModel::FM_WarmRhodes,     Synth::EngineMode::FM},
            {14, "FM CLAVINET", Synth::SynthModel::FM_Clavinet,       Synth::EngineMode::FM},
            {15, "FM ORGAN",    Synth::SynthModel::FM_DrawbarOrgan,   Synth::EngineMode::FM},
            {16, "PPG WAVE",    Synth::SynthModel::PPG_Wave,          Synth::EngineMode::Wavetable},
            {17, "WAVE VOX",    Synth::SynthModel::WT_VoxPad,         Synth::EngineMode::Wavetable},
            {18, "CZ BASS",     Synth::SynthModel::CZ_Bass,           Synth::EngineMode::PhaseDist},
            {19, "CZ RESO",     Synth::SynthModel::CZ_Reso,           Synth::EngineMode::PhaseDist},
            {20, "KS GUITAR",   Synth::SynthModel::KS_Guitar,         Synth::EngineMode::Pluck},
            {21, "KS KOTO",     Synth::SynthModel::KS_Koto,           Synth::EngineMode::Pluck},
            {22, "B3 JAZZ",     Synth::SynthModel::B3_Jazz,           Synth::EngineMode::Additive},
            {23, "B3 GOSPEL",   Synth::SynthModel::B3_Gospel,         Synth::EngineMode::Additive},
        };
        for (const Expect& e : factory) {
            Synth::Preset out;
            Synth::DecodedSysEx dump = request(e.slot, out);
            assert(dump.isValid && dump.type == Synth::SysExCommand::PatchDumpV2);
            assert(std::strncmp(out.name, e.name, Synth::PATCH_NAME_MAX) == 0);
            assert(out.voicing == e.model);
            assert(out.engineMode == e.mode);
        }

        // FM Warm Rhodes keeps its own tremolo rate (v1 hardcoded 4.5 Hz)
        Synth::Preset rhodes;
        request(13, rhodes);
        assert(std::abs(rhodes.tremoloRateHz - 4.2f) < 0.01f);
        assert(std::abs(rhodes.tremoloDepth - 0.38f) < 0.001f);

        // User slots #24 and #32
        Synth::Preset user;
        assert(request(Synth::USER_PRESET_SLOT, user).isValid);
        assert(std::strcmp(user.name, "USER 01") == 0);
        assert(request(32, user).isValid);
        assert(std::strcmp(user.name, "USER 09") == 0);

        // Slot 33 (invalid slot beyond 32)
        Synth::DecodedSysEx dumpInv = request(33, user);
        assert(dumpInv.isValid);
        assert(dumpInv.type == Synth::SysExCommand::Handshake);
        assert(dumpInv.refCommand == Synth::SysExCommand::RequestDump);
        assert(dumpInv.handshakeStatus == Synth::HandshakeStatus::NackInvalidSlot);

        std::cout << "  Passed (Factory 1..23 and user 24..32 dumps reply as PatchDumpV2 with voicing, engine and FM tremolo rate; slot 33 rejected)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 7: Malformed / Truncated SysEx Rejection & Stability
    // -------------------------------------------------------------
    std::cout << "\nTest 7: Stream Corruption, Truncation & Framing Safety..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);
        Synth::MidiParser parser;

        // 7a. Truncated packet (F0 without F7)
        uint8_t truncated[] = {0xF0, 0x7D, 0x01, 0x01, 0x01};
        parser.parseBuffer(truncated, sizeof(truncated), engine);
        // Next valid note on should still work cleanly
        parser.parseByte(0x90, engine);
        parser.parseByte(60, engine);
        parser.parseByte(100, engine);
        assert(engine.getActiveVoiceCount() == 1);
        engine.panic();

        // 7b. Interleaved real-time MIDI clock (0xF8) inside a fast SysEx stream
        uint8_t fastUpdateInterleaved[] = {
            0xF0, 0x7D, 0xF8, 0x01, 0x01, 0xF8, 0x01, 0x7F, 0x7F, 0x00, 0xF8, 0x00, 0xF7
        };
        parser.parseBuffer(fastUpdateInterleaved, sizeof(fastUpdateInterleaved), engine);
        assert(engine.getCutoffHz() > 19000.0f);

        // 7c. Wrong Manufacturer ID / Family ID
        uint8_t wrongMfg[] = {0xF0, 0x42, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0xF7};
        parser.parseBuffer(wrongMfg, sizeof(wrongMfg), engine);
        // Should not modify engine cutoff
        engine.setCutoffHz(1000.0f);
        parser.parseBuffer(wrongMfg, sizeof(wrongMfg), engine);
        assert(engine.getCutoffHz() == 1000.0f);

        // 7d. Buffer flood protection (> 300 bytes without EOX)
        parser.parseByte(0xF0, engine);
        for (int i = 0; i < 400; ++i) {
            parser.parseByte(0x20, engine);
        }
        parser.parseByte(0xF7, engine);

        // Engine must remain responsive
        parser.parseByte(0x90, engine);
        parser.parseByte(64, engine);
        parser.parseByte(90, engine);
        assert(engine.getActiveVoiceCount() == 1);
        engine.panic();

        std::cout << "  Passed (Engine robust against truncated streams, floods, and bad headers)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 8: VoiceMode, Portamento, Legato & Glide Roundtrip
    // -------------------------------------------------------------
    std::cout << "\nTest 8: Preserves VoiceMode, Portamento, Legato, and GlideTime in SysEx Patch roundtrip..." << std::endl;
    {
        Synth::Preset p{};
        p.name = "ACID 303";
        p.model = Synth::SynthModel::TB303;
        p.waveform = Synth::Waveform::Sawtooth;
        p.voiceMode = Synth::VoiceMode::Monophonic;
        p.portamentoEnabled = true;
        p.legatoEnabled = true;
        p.glideTimeMs = 350.0f;
        p.chorusEnabled = true;
        p.chorusRateHz = 2.0f;
        p.chorusDepthMs = 3.5f;
        p.chorusMix = 0.6f;
        p.cutoffHz = 1500.0f;
        p.resonanceQ = 12.0f;
        p.subOscLevel = 0.5f;
        p.detuneCents = 10.0f;
        p.drive = 2.0f;
        p.filterEnvAmount = 0.8f;
        p.ampAttackMs = 10.0f;
        p.ampDecayMs = 200.0f;
        p.ampSustain = 0.0f;
        p.ampReleaseMs = 50.0f;
        p.filterAttackMs = 10.0f;
        p.filterDecayMs = 300.0f;
        p.filterSustain = 0.2f;
        p.filterReleaseMs = 100.0f;

        Synth::RawPatchData raw{};
        Synth::SysExProtocol::presetToRawPatch(p, raw);

        assert(raw.voiceMode == 1);
        assert((raw.flags & 0x01) != 0); // chorus
        assert((raw.flags & 0x02) != 0); // portamento
        assert((raw.flags & 0x04) != 0); // legato
        assert(raw.glideTime > 0);

        Synth::Preset restored{};
        char nameBuf[16];
        Synth::SysExProtocol::rawPatchToPreset(raw, restored, nameBuf);

        assert(restored.voiceMode == Synth::VoiceMode::Monophonic);
        assert(restored.portamentoEnabled == true);
        assert(restored.legatoEnabled == true);
        assert(std::abs(restored.glideTimeMs - 350.0f) < 10.0f);
        assert(restored.chorusEnabled == true);

        std::cout << "  Passed (VoiceMode, Portamento, Legato, and GlideTime preserved in roundtrip)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 9: Command 0x01 Silence on Malformed Packet
    // -------------------------------------------------------------
    std::cout << "\nTest 9: FastParamUpdate (0x01) is fire-and-forget with NO handshake response on error..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);

        bool callbackInvoked = false;
        engine.setOnSysExSend([&callbackInvoked](const uint8_t* data, size_t len) {
            callbackInvoked = true;
        });

        // Malformed 0x01 packet (invalid length: 6 bytes instead of 10)
        uint8_t malformedFastParam[] = {0xF0, 0x7D, 0x01, 0x01, 0x01, 0xF7};
        engine.handleSysEx(malformedFastParam, sizeof(malformedFastParam));

        assert(!callbackInvoked); // Must NOT emit NACK handshake
        std::cout << "  Passed (Malformed Command 0x01 silently dropped without NACK)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 10: Dynamic Chorus & Mono State in Slot 0 Dump and User Save
    // -------------------------------------------------------------
    std::cout << "\nTest 10: Active RAM Dump & SaveUserSlot dynamic chorus and mono parameters..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);

        engine.setChorusRateHz(3.2f);
        engine.setChorusDepthMs(4.1f);
        engine.setChorusMix(0.85f);
        engine.setVoiceMode(Synth::VoiceMode::Monophonic);
        engine.setPortamentoEnabled(true);
        engine.setGlideTimeMs(420.0f);

        std::vector<uint8_t> capturedResponse;
        engine.setOnSysExSend([&capturedResponse](const uint8_t* data, size_t len) {
            capturedResponse.assign(data, data + len);
        });

        // Request slot 0 dump
        uint8_t reqDumpRam[] = {0xF0, 0x7D, 0x01, 0x04, 0x00, 0xF7};
        engine.handleSysEx(reqDumpRam, sizeof(reqDumpRam));

        assert(!capturedResponse.empty());
        Synth::DecodedSysEx decoded = Synth::SysExProtocol::decodeSysEx(capturedResponse.data(), capturedResponse.size());
        assert(decoded.isValid);
        assert(decoded.type == Synth::SysExCommand::PatchDumpV2);

        Synth::Preset ramPreset = Synth::getPresets()[Synth::FACTORY_PRESET_COUNT];
        char ramName[Synth::PATCH_NAME_MAX + 1];
        assert(Synth::SysExProtocol::decodePatchBody(decoded.patchBody.data(), decoded.patchBody.size(), ramPreset, ramName));
        assert(std::abs(ramPreset.chorusRateHz - 3.2f) < 0.1f);
        assert(std::abs(ramPreset.chorusDepthMs - 4.1f) < 0.1f);
        assert(ramPreset.voiceMode == Synth::VoiceMode::Monophonic);
        assert(ramPreset.portamentoEnabled == true);
        assert(std::abs(ramPreset.glideTimeMs - 420.0f) < 10.0f);

        std::cout << "  Passed (Dynamic chorus and mono parameters correctly reflected in slot 0 dump)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 11: FastParamUpdate targeting User Slot #24 updates User Preset
    // -------------------------------------------------------------
    std::cout << "\nTest 11: FastParamUpdate targeting User Slot #24 updates User Preset..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);

        // Load Preset 0 (Juno-60)
        engine.loadPresetIndex(0);
        float junoCutoff = engine.getCutoffHz();

        // Send FastParamUpdate targeting Slot 24 (User Slot #24 / USER 01)
        // [F0] [7D] [01] [01] [ParamID=0x01 Cutoff] [MSB=0x7F] [LSB=0x7F] [Slot=0x18] [Reserved=0x00] [F7]
        uint8_t fastUpdateSlot16[] = {0xF0, 0x7D, 0x01, 0x01, 0x01, 0x7F, 0x7F, Synth::USER_PRESET_SLOT, 0x00, 0xF7};
        engine.handleSysEx(fastUpdateSlot16, sizeof(fastUpdateSlot16));

        // Active Juno cutoff should remain unchanged
        assert(std::abs(engine.getCutoffHz() - junoCutoff) < 1.0f);

        // User preset cutoff should be updated to max (~20000 Hz)
        assert(engine.getUserPreset().cutoffHz > 19000.0f);

        // Targeting a read-only factory slot (e.g. Slot 7 / Casio PT-10) should be ignored
        uint8_t fastUpdateFactorySlot[] = {0xF0, 0x7D, 0x01, 0x01, 0x01, 0x10, 0x10, 0x07, 0x00, 0xF7};
        engine.handleSysEx(fastUpdateFactorySlot, sizeof(fastUpdateFactorySlot));
        assert(std::abs(engine.getCutoffHz() - junoCutoff) < 1.0f);

        std::cout << "  Passed (FastParamUpdate targeting Slot 24 updates User Preset without corrupting active factory preset; factory slot write ignored)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 12: Code Review Regression Suite (CRIT-02, CRIT-07, HIGH-05)
    // -------------------------------------------------------------
    std::cout << "\nTest 12: FM User Slot round-trip, softClip continuity, and live Key/Scale noteOff..." << std::endl;
    {
        // 1. CRIT-07: FM User Preset round-trip preserves EngineMode::FM
        Synth::Preset fmUserPreset{};
        fmUserPreset.model = Synth::SynthModel::User_01; // User Slot #24 (non-factory FM model)
        fmUserPreset.name = "FM BELL USER";
        fmUserPreset.engineMode = Synth::EngineMode::FM;
        fmUserPreset.modDepth = 8.4f;
        fmUserPreset.modRatio = 3.5f;
        fmUserPreset.modFeedback = 0.6f;

        Synth::RawPatchData rawFm{};
        Synth::SysExProtocol::presetToRawPatch(fmUserPreset, rawFm);
        assert((rawFm.flags & 0x08) != 0);

        Synth::Preset restoredFm{};
        char restoredName[16];
        Synth::SysExProtocol::rawPatchToPreset(rawFm, restoredFm, restoredName);
        assert(restoredFm.engineMode == Synth::EngineMode::FM);
        assert(std::abs(restoredFm.modDepth - 8.4f) < 0.05f);
        assert(std::abs(restoredFm.modRatio - 3.5f) < 0.05f);
        assert(std::abs(restoredFm.modFeedback - 0.6f) < 0.05f);

        // 2. CRIT-02: ParameterScaler::softClip continuity across x = +/-3.0f
        float leftOf3  = Synth::ParameterScaler::softClip(2.9999f);
        float rightOf3 = Synth::ParameterScaler::softClip(3.0001f);
        assert(std::abs(rightOf3 - leftOf3) < 0.001f);

        // 3. HIGH-05: Dynamic Root Key / Scale change while note is held releases cleanly
        Synth::SynthEngine engine;
        engine.init(44100.0f);
        engine.setScaleMode(Synth::ScaleMode::Major);
        engine.setRootKey(0); // C Major
        engine.noteOn(61, 100); // C#4 quantized to C4 (60)
        assert(engine.getActiveVoiceCount() == 1);

        // Change Root Key while key 61 is physically held down
        engine.setRootKey(2); // D Major (where 61 quantizes to 61)
        engine.noteOff(61);   // Release raw key 61
        // Render through release phase and verify voice terminates cleanly
        std::vector<int16_t> buf(44100 * 2, 0);
        engine.processBlock(buf.data(), 44100);
        assert(engine.getActiveVoiceCount() == 0);

        std::cout << "  Passed (FM User Slot preserves EngineMode::FM, softClip is continuous, and held notes release after live scale/key changes)" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 13: PatchDumpV2 parameter-list format (ADR-0006)
    // -------------------------------------------------------------
    std::cout << "\nTest 13: PatchDumpV2 handshake, unknown/missing/duplicate IDs, malformed bodies, v1 NVS migration..." << std::endl;
    {
        using namespace Synth;

        // Builds a v2 dump from a name and raw entries (each entry = its bytes, 3 or 5 of them)
        auto buildDump = [](uint8_t slot, const char* name, const std::vector<std::vector<uint8_t>>& entries) {
            std::vector<uint8_t> m = {SYSEX_START, SYSEX_MANUFACTURER_ID, SYSEX_FAMILY_ID,
                                      static_cast<uint8_t>(SysExCommand::PatchDumpV2), slot,
                                      static_cast<uint8_t>(std::strlen(name))};
            m.insert(m.end(), name, name + std::strlen(name));
            for (const auto& e : entries) m.insert(m.end(), e.begin(), e.end());
            m.push_back(SysExProtocol::computeChecksum(&m[4], m.size() - 4));
            m.push_back(SYSEX_END);
            return m;
        };
        auto entry = [](ParamId id, uint16_t v14) {
            return std::vector<uint8_t>{static_cast<uint8_t>(id), static_cast<uint8_t>((v14 >> 7) & 0x7F),
                                        static_cast<uint8_t>(v14 & 0x7F)};
        };
        auto lastHandshake = [](const SynthEngine& e) {
            const auto& r = e.getLastSysExResponse();
            return SysExProtocol::decodeSysEx(r.data(), r.size());
        };
        auto v14 = [](ParamId id, float v) { return SysExProtocol::paramToValue14(*SysExProtocol::findPatchParam(id), v); };

        SynthEngine engine;
        engine.init(44100.0f);

        // Full round trip: every table parameter survives encode -> engine -> RequestDump-style encode
        Preset fm = getPresets()[static_cast<size_t>(SynthModel::FM_DrawbarOrgan)];
        uint8_t buf[MAX_SYSEX_MESSAGE_SIZE];
        size_t n = SysExProtocol::encodePatchDumpV2(26, fm, buf, sizeof(buf));
        size_t paramCount = 0;
        SysExProtocol::patchParams(paramCount);
        assert(n == PATCH_V2_FRAMING_SIZE + 1 + std::strlen(fm.name) + 3 * paramCount);
        engine.handleSysEx(buf, n);
        DecodedSysEx ack = lastHandshake(engine);
        assert(ack.type == SysExCommand::Handshake && ack.refCommand == SysExCommand::PatchDumpV2);
        assert(ack.handshakeStatus == HandshakeStatus::AckSuccess);
        const Preset& u18 = engine.getUserPreset(26);
        assert(u18.engineMode == EngineMode::FM);
        assert(u18.voicing == SynthModel::FM_DrawbarOrgan);
        assert(std::abs(u18.tremoloRateHz - 6.2f) < 0.01f);
        assert(std::abs(u18.modFeedback - 0.45f) < 0.001f);
        assert(u18.model == static_cast<SynthModel>(FACTORY_PRESET_COUNT + 2));
        engine.loadPresetIndex(FACTORY_PRESET_COUNT + 2);
        assert(engine.getEngineMode() == EngineMode::FM);
        assert(std::abs(engine.getTremoloRateHz() - 6.2f) < 0.01f);

        // Unknown IDs (incl. an extended 0x7F entry) and global IDs are skipped; missing IDs take the
        // user-slot defaults; a duplicate ID resolves to the last one
        auto dump = buildDump(25, "SPARSE", {
            entry(ParamId::DetuneCents, v14(ParamId::DetuneCents, 10.0f)),
            {0x7E, 0x12, 0x34},                         // unknown ID
            {PARAM_ID_EXTENDED, 0x01, 0x02, 0x7F, 0x7F}, // unknown extended ID
            entry(ParamId::MasterVolume, 0),             // global: ignored in a dump
            entry(ParamId::Waveform, 2),
            entry(ParamId::DetuneCents, v14(ParamId::DetuneCents, 33.0f)),
        });
        const float volBefore = engine.getMasterVolume();
        engine.handleSysEx(dump.data(), dump.size());
        assert(lastHandshake(engine).handshakeStatus == HandshakeStatus::AckSuccess);
        const Preset& u17 = engine.getUserPreset(25);
        const Preset& def = getPresets()[FACTORY_PRESET_COUNT + 1];
        assert(std::strcmp(u17.name, "SPARSE") == 0);
        assert(std::abs(u17.detuneCents - 33.0f) < 0.01f);
        assert(u17.waveform == Waveform::Triangle);
        assert(u17.cutoffHz == def.cutoffHz && u17.ampReleaseMs == def.ampReleaseMs);
        assert(u17.engineMode == EngineMode::Subtractive);
        assert(engine.getMasterVolume() == volBefore);

        // Factory slot rejected; malformed bodies are NACKed and leave the slot untouched
        dump = buildDump(3, "NOPE", {entry(ParamId::Waveform, 1)});
        engine.handleSysEx(dump.data(), dump.size());
        assert(lastHandshake(engine).handshakeStatus == HandshakeStatus::NackInvalidSlot);

        dump = buildDump(25, "BAD", {entry(ParamId::Waveform, 1)});
        dump.insert(dump.end() - 2, 0x01);                                   // dangling partial entry
        dump[dump.size() - 2] = SysExProtocol::computeChecksum(&dump[4], dump.size() - 6);
        engine.handleSysEx(dump.data(), dump.size());
        DecodedSysEx nack = lastHandshake(engine);
        assert(nack.refCommand == SysExCommand::PatchDumpV2 && nack.handshakeStatus == HandshakeStatus::NackCorrupted);

        dump = buildDump(25, "TWELVECHARSS", {});                            // name longer than 11
        engine.handleSysEx(dump.data(), dump.size());
        assert(lastHandshake(engine).handshakeStatus == HandshakeStatus::NackCorrupted);

        dump = buildDump(25, "CSUM", {entry(ParamId::Waveform, 1)});
        dump[dump.size() - 2] ^= 0x01;
        engine.handleSysEx(dump.data(), dump.size());
        assert(lastHandshake(engine).handshakeStatus == HandshakeStatus::NackChecksumErr);
        assert(std::strcmp(engine.getUserPreset(25).name, "SPARSE") == 0);

        // Live edit buffer (slot 0) applies without touching any user slot
        dump = buildDump(0, "LIVE", {entry(ParamId::DetuneCents, v14(ParamId::DetuneCents, 42.0f))});
        engine.handleSysEx(dump.data(), dump.size());
        assert(lastHandshake(engine).handshakeStatus == HandshakeStatus::AckSuccess);
        assert(std::abs(engine.getDetuneCents() - 42.0f) < 0.01f);

        // New FastParamUpdate IDs: live and targeted at a user slot
        uint8_t fp[10];
        SysExProtocol::encodeFastParamUpdate(ParamId::TremoloRate, v14(ParamId::TremoloRate, 7.5f), 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(std::abs(engine.getTremoloRateHz() - 7.5f) < 0.01f);
        SysExProtocol::encodeFastParamUpdate(ParamId::LegatoEnabled, 1, 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(engine.isLegatoEnabled());
        SysExProtocol::encodeFastParamUpdate(ParamId::LegatoEnabled, 0, 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(!engine.isLegatoEnabled());
        SysExProtocol::encodeFastParamUpdate(ParamId::ModRatio, v14(ParamId::ModRatio, 3.0f), 28, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(std::abs(engine.getUserPreset(28).modRatio - 3.0f) < 0.01f);
        // EngineMode / Voicing live: a generic patch takes the DX voicing in FM and gets its generic
        // voicing back in Subtractive; neither change touches other live state
        assert(engine.getEngineMode() == EngineMode::Subtractive && engine.getVoicingModel() == SynthModel::Count);
        SysExProtocol::encodeFastParamUpdate(ParamId::EngineMode, 1, 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(engine.getEngineMode() == EngineMode::FM && engine.getVoicingModel() == SynthModel::DX_EPiano);
        assert(std::abs(engine.getDetuneCents() - 42.0f) < 0.01f);
        SysExProtocol::encodeFastParamUpdate(ParamId::EngineMode, 0, 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(engine.getEngineMode() == EngineMode::Subtractive && engine.getVoicingModel() == SynthModel::Count);
        SysExProtocol::encodeFastParamUpdate(ParamId::Voicing, static_cast<uint16_t>(SynthModel::TB303) + 1, 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(engine.getVoicingModel() == SynthModel::TB303 && engine.getVoicing().accent);

        // A factory FM preset switched to Subtractive stays there when its voicing changes, and a slot-0
        // dump reports the live engine
        engine.loadPresetIndex(static_cast<size_t>(SynthModel::DX_EPiano));
        SysExProtocol::encodeFastParamUpdate(ParamId::EngineMode, 0, 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        SysExProtocol::encodeFastParamUpdate(ParamId::Voicing, 1, 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(engine.getEngineMode() == EngineMode::Subtractive && engine.getVoicingModel() == SynthModel::Juno60);
        const uint8_t reqLive[] = {0xF0, 0x7D, 0x01, 0x04, 0x00, 0xF7};
        std::vector<uint8_t> reply;
        engine.handleSysEx(reqLive, sizeof(reqLive), reply);
        DecodedSysEx liveDump = SysExProtocol::decodeSysEx(reply.data(), reply.size());
        assert(liveDump.isValid && liveDump.type == SysExCommand::PatchDumpV2);
        Preset liveP = getPresets()[FACTORY_PRESET_COUNT];
        char liveName[PATCH_NAME_MAX + 1];
        SysExProtocol::decodePatchBody(liveDump.patchBody.data(), liveDump.patchBody.size(), liveP, liveName);
        assert(liveP.engineMode == EngineMode::Subtractive && liveP.voicing == SynthModel::Juno60);

        // A redundant EngineMode on a Casio preset keeps its tone and pulse width
        engine.loadPresetIndex(static_cast<size_t>(SynthModel::CasioPT10_Piano));
        const float pw = engine.getPulseWidth();
        SysExProtocol::encodeFastParamUpdate(ParamId::EngineMode, 0, 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(engine.getCasioTone() == CasioTone::Piano && engine.getPulseWidth() == pw);

        // On/off enums keep the old leniency: the 7-bit form F0 7D 01 01 30 01 F7 (value14 = 128) is Mono
        const uint8_t mono7[] = {0xF0, 0x7D, 0x01, 0x01, 0x30, 0x01, 0xF7};
        engine.handleSysEx(mono7, sizeof(mono7));
        assert(engine.getVoiceMode() == VoiceMode::Monophonic);

        // Global IDs targeting an inactive user slot are ignored, as before
        const float vol = engine.getMasterVolume();
        SysExProtocol::encodeFastParamUpdate(ParamId::MasterVolume, 0, 25, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(engine.getMasterVolume() == vol);
        // Existing IDs keep their scaling (full-scale cutoff is 20 kHz)
        SysExProtocol::encodeFastParamUpdate(ParamId::Cutoff, 16383, 0, fp, sizeof(fp));
        engine.handleSysEx(fp, sizeof(fp));
        assert(std::abs(engine.getCutoffHz() - 20000.0f) < 1.0f);

        // v1 NVS migration: a slot with only a legacy 48-byte blob loads it; its next save writes v2
        Preset legacy = getPresets()[static_cast<size_t>(SynthModel::Minimoog)];
        legacy.name = "OLD MOOG";
        RawPatchData raw;
        SysExProtocol::presetToRawPatch(legacy, raw);
        std::vector<uint8_t> v2Store;
        bool v1Read = false;
        SynthEngine migrated;
        migrated.setStorageCallbacks(
            [&](uint8_t slot, const uint8_t* data, size_t size) {
                if (slot == 27) v2Store.assign(data, data + size);
                return true;
            },
            [&](uint8_t slot, uint8_t* out, size_t maxSize) -> size_t {
                if (slot != 27 || v2Store.empty() || v2Store.size() > maxSize) return 0;
                std::memcpy(out, v2Store.data(), v2Store.size());
                return v2Store.size();
            },
            [&](uint8_t slot, uint8_t* out, size_t size) {
                if (slot != 27 || size != sizeof(raw)) return false;
                std::memcpy(out, &raw, sizeof(raw));
                v1Read = true;
                return true;
            });
        migrated.init(44100.0f);
        assert(v1Read && v2Store.empty()); // nothing rewritten at boot
        assert(std::strcmp(migrated.getUserPreset(27).name, "OLD MOOG") == 0);
        assert(migrated.getUserPreset(27).voicing == SynthModel::Minimoog);
        assert(migrated.saveUserPresetToStorage(27) && !v2Store.empty());
        v1Read = false;
        SynthEngine rebooted;
        rebooted.setStorageCallbacks(nullptr,
            [&](uint8_t slot, uint8_t* out, size_t maxSize) -> size_t {
                if (slot != 27 || v2Store.size() > maxSize) return 0;
                std::memcpy(out, v2Store.data(), v2Store.size());
                return v2Store.size();
            },
            [&](uint8_t slot, uint8_t* out, size_t size) {
                if (slot != 27 || size != sizeof(raw)) return false;
                std::memcpy(out, &raw, sizeof(raw)); // still present, but the v2 body wins
                v1Read = true;
                return true;
            });
        rebooted.init(44100.0f);
        assert(!v1Read);
        assert(std::strcmp(rebooted.getUserPreset(27).name, "OLD MOOG") == 0);
        assert(rebooted.getUserPreset(27).voicing == SynthModel::Minimoog);

        std::cout << "  Passed (v2 dumps ACK as 0x05, skip unknown/global IDs, default missing ones, reject malformed bodies; v1 NVS blobs migrate on next save)" << std::endl;
    }

    std::cout << "\n==================================================" << std::endl;
    std::cout << "All SysEx Protocol and Storage Tests Passed 100%!" << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
