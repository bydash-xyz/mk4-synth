#include "SysExProtocol.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace Synth {

// Models that existed when the legacy v1 dump (command 0x02) was current: indices 0..14 are factory
// models in both bank layouts; 15..31 were the user slots then and are factory models now
static constexpr uint8_t kV1FactoryModels = 15;

size_t SysExProtocol::pack7Bit(const uint8_t* inRaw, size_t inLen, uint8_t* outMidi, size_t maxOutLen) {
    size_t inIdx = 0;
    size_t outIdx = 0;

    while (inIdx < inLen) {
        size_t count = std::min(size_t(7), inLen - inIdx);
        if (outIdx + 1 + count > maxOutLen) {
            return outIdx; // buffer overflow protection
        }

        uint8_t header = 0;
        size_t headerIdx = outIdx++;

        for (size_t j = 0; j < count; ++j) {
            uint8_t b = inRaw[inIdx++];
            if (b & 0x80) {
                header |= (1 << j);
            }
            outMidi[outIdx++] = b & 0x7F;
        }
        outMidi[headerIdx] = header & 0x7F;
    }

    return outIdx;
}

size_t SysExProtocol::unpack7Bit(const uint8_t* inMidi, size_t inLen, uint8_t* outRaw, size_t maxOutLen) {
    size_t inIdx = 0;
    size_t outIdx = 0;

    while (inIdx < inLen) {
        uint8_t header = inMidi[inIdx++];
        size_t count = std::min(size_t(7), inLen - inIdx);

        for (size_t j = 0; j < count; ++j) {
            if (outIdx >= maxOutLen) {
                return outIdx;
            }
            uint8_t b = inMidi[inIdx++];
            uint8_t bit = (header >> j) & 0x01;
            outRaw[outIdx++] = (b & 0x7F) | (bit << 7);
        }
    }

    return outIdx;
}

uint8_t SysExProtocol::computeChecksum(const uint8_t* data, size_t len) {
    uint8_t c = 0;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
    }
    return c & 0x7F;
}

size_t SysExProtocol::encodeFastParamUpdate(ParamId param, uint16_t val14, uint8_t slot, uint8_t* outBuffer, size_t maxLen) {
    if (maxLen < 10) return 0;

    outBuffer[0] = SYSEX_START;
    outBuffer[1] = SYSEX_MANUFACTURER_ID;
    outBuffer[2] = SYSEX_FAMILY_ID;
    outBuffer[3] = static_cast<uint8_t>(SysExCommand::FastParamUpdate);
    outBuffer[4] = static_cast<uint8_t>(param) & 0x7F;
    outBuffer[5] = (val14 >> 7) & 0x7F;
    outBuffer[6] = val14 & 0x7F;
    outBuffer[7] = slot & 0x7F;
    outBuffer[8] = 0x00; // Reserved
    outBuffer[9] = SYSEX_END;

    return 10;
}

size_t SysExProtocol::encodePatchDump(uint8_t slot, const RawPatchData& patch, uint8_t* outBuffer, size_t maxLen) {
    if (maxLen < FULL_DUMP_PACKET_SIZE) return 0;

    outBuffer[0] = SYSEX_START;
    outBuffer[1] = SYSEX_MANUFACTURER_ID;
    outBuffer[2] = SYSEX_FAMILY_ID;
    outBuffer[3] = static_cast<uint8_t>(SysExCommand::PatchDump);
    outBuffer[4] = slot & 0x7F;

    size_t packedBytes = pack7Bit(
        reinterpret_cast<const uint8_t*>(&patch),
        sizeof(RawPatchData),
        &outBuffer[5],
        PACKED_PATCH_SIZE
    );

    if (packedBytes != PACKED_PATCH_SIZE) {
        return 0;
    }

    size_t checksumPos = 5 + PACKED_PATCH_SIZE;
    outBuffer[checksumPos] = computeChecksum(&outBuffer[4], 1 + PACKED_PATCH_SIZE);
    outBuffer[checksumPos + 1] = SYSEX_END;

    return checksumPos + 2;
}

size_t SysExProtocol::encodeSaveUserSlot(uint8_t slot, uint8_t* outBuffer, size_t maxLen) {
    if (maxLen < 6) return 0;

    outBuffer[0] = SYSEX_START;
    outBuffer[1] = SYSEX_MANUFACTURER_ID;
    outBuffer[2] = SYSEX_FAMILY_ID;
    outBuffer[3] = static_cast<uint8_t>(SysExCommand::SaveUserSlot);
    outBuffer[4] = slot & 0x7F;
    outBuffer[5] = SYSEX_END;

    return 6;
}

size_t SysExProtocol::encodeRequestPatchDump(uint8_t slot, uint8_t* outBuffer, size_t maxLen) {
    if (maxLen < 6) return 0;

    outBuffer[0] = SYSEX_START;
    outBuffer[1] = SYSEX_MANUFACTURER_ID;
    outBuffer[2] = SYSEX_FAMILY_ID;
    outBuffer[3] = static_cast<uint8_t>(SysExCommand::RequestDump);
    outBuffer[4] = slot & 0x7F;
    outBuffer[5] = SYSEX_END;

    return 6;
}

size_t SysExProtocol::encodeHandshake(SysExCommand refCmd, HandshakeStatus status, uint8_t* outBuffer, size_t maxLen) {
    if (maxLen < 7) return 0;

    outBuffer[0] = SYSEX_START;
    outBuffer[1] = SYSEX_MANUFACTURER_ID;
    outBuffer[2] = SYSEX_FAMILY_ID;
    outBuffer[3] = static_cast<uint8_t>(SysExCommand::Handshake);
    outBuffer[4] = static_cast<uint8_t>(refCmd) & 0x7F;
    outBuffer[5] = static_cast<uint8_t>(status) & 0x7F;
    outBuffer[6] = SYSEX_END;

    return 7;
}

DecodedSysEx SysExProtocol::decodeSysEx(const uint8_t* data, size_t len) {
    DecodedSysEx res;
    res.isValid = false;

    if (len < 4) {
        res.errorStatus = HandshakeStatus::NackCorrupted;
        return res;
    }

    if (data[0] != SYSEX_START || data[len - 1] != SYSEX_END) {
        res.errorStatus = HandshakeStatus::NackCorrupted;
        return res;
    }

    if (data[1] != SYSEX_MANUFACTURER_ID || data[2] != SYSEX_FAMILY_ID) {
        res.errorStatus = HandshakeStatus::NackUnsupported;
        return res;
    }

    uint8_t cmd = data[3];

    switch (cmd) {
        case static_cast<uint8_t>(SysExCommand::FastParamUpdate): {
            // Minimal 7-bit: F0 7D 01 01 <Param> <Val7> F7 (7 bytes)
            // Minimal 14-bit: F0 7D 01 01 <Param> <MSB> <LSB> F7 (8 bytes)
            // Standard: F0 7D 01 01 <Param> <MSB> <LSB> <Slot> <Reserved> F7 (10 bytes)
            if (len < 7) {
                res.errorStatus = HandshakeStatus::NackCorrupted;
                return res;
            }

            res.type = SysExCommand::FastParamUpdate;
            res.fastParam.paramId = static_cast<ParamId>(data[4]);
            if (len == 7) {
                res.fastParam.value14 = static_cast<uint16_t>((data[5] & 0x7F) << 7);
                res.fastParam.slot = 0;
                res.fastParam.flags = 0;
            } else {
                res.fastParam.value14 = static_cast<uint16_t>(((data[5] & 0x7F) << 7) | (data[6] & 0x7F));
                res.fastParam.slot = (len >= 9 && data[7] != SYSEX_END) ? data[7] : 0;
                res.fastParam.flags = (len >= 10 && data[8] != SYSEX_END) ? data[8] : 0;
            }
            res.isValid = true;
            break;
        }

        case static_cast<uint8_t>(SysExCommand::PatchDump): {
            if (len != FULL_DUMP_PACKET_SIZE) {
                res.errorStatus = HandshakeStatus::NackCorrupted;
                return res;
            }

            uint8_t slot = data[4];
            const uint8_t* packedPayload = &data[5];
            uint8_t receivedChecksum = data[5 + PACKED_PATCH_SIZE];

            uint8_t expectedChecksum = computeChecksum(&data[4], 1 + PACKED_PATCH_SIZE);
            if (receivedChecksum != expectedChecksum) {
                res.errorStatus = HandshakeStatus::NackChecksumErr;
                return res;
            }

            size_t unpacked = unpack7Bit(
                packedPayload,
                PACKED_PATCH_SIZE,
                reinterpret_cast<uint8_t*>(&res.patchData),
                sizeof(RawPatchData)
            );

            if (unpacked != sizeof(RawPatchData)) {
                res.errorStatus = HandshakeStatus::NackCorrupted;
                return res;
            }

            res.type = SysExCommand::PatchDump;
            res.slot = slot;
            res.isValid = true;
            break;
        }

        case static_cast<uint8_t>(SysExCommand::SaveUserSlot): {
            if (len < 6) {
                res.errorStatus = HandshakeStatus::NackCorrupted;
                return res;
            }

            res.type = SysExCommand::SaveUserSlot;
            res.targetSlot = data[4];
            res.isValid = true;
            break;
        }

        case static_cast<uint8_t>(SysExCommand::RequestDump): {
            if (len < 6) {
                res.errorStatus = HandshakeStatus::NackCorrupted;
                return res;
            }

            res.type = SysExCommand::RequestDump;
            res.targetSlot = data[4];
            res.isValid = true;
            break;
        }

        case static_cast<uint8_t>(SysExCommand::PatchDumpV2): {
            // F0 7D 01 05 <slot> <body...> <xor> F7
            if (len < PATCH_V2_FRAMING_SIZE + 1 || len > MAX_SYSEX_MESSAGE_SIZE) {
                res.errorStatus = HandshakeStatus::NackCorrupted;
                return res;
            }
            const size_t checksumPos = len - 2;
            if (data[checksumPos] != computeChecksum(&data[4], checksumPos - 4)) {
                res.errorStatus = HandshakeStatus::NackChecksumErr;
                return res;
            }
            const uint8_t* body = &data[5];
            const size_t bodyLen = checksumPos - 5;
            if (!isValidPatchBody(body, bodyLen)) {
                res.errorStatus = HandshakeStatus::NackCorrupted;
                return res;
            }
            res.type = SysExCommand::PatchDumpV2;
            res.slot = data[4];
            res.patchBody.assign(body, body + bodyLen);
            res.isValid = true;
            break;
        }

        case static_cast<uint8_t>(SysExCommand::Handshake): {
            if (len < 7) {
                res.errorStatus = HandshakeStatus::NackCorrupted;
                return res;
            }

            res.type = SysExCommand::Handshake;
            res.refCommand = static_cast<SysExCommand>(data[4]);
            res.handshakeStatus = static_cast<HandshakeStatus>(data[5]);
            res.isValid = true;
            break;
        }

        default:
            res.errorStatus = HandshakeStatus::NackUnsupported;
            break;
    }

    return res;
}

void SysExProtocol::presetToRawPatch(const Preset& preset, RawPatchData& outRaw) {
    std::memset(&outRaw, 0, sizeof(RawPatchData));

    if (preset.name) {
        std::strncpy(outRaw.name, preset.name, sizeof(outRaw.name) - 1);
    }
    outRaw.name[sizeof(outRaw.name) - 1] = '\0';

    outRaw.model = static_cast<uint8_t>(preset.model);
    outRaw.waveform = static_cast<uint8_t>(preset.waveform);
    outRaw.voiceMode = static_cast<uint8_t>(preset.voiceMode);

    const bool isFm = presetEngineMode(preset) == EngineMode::FM;

    uint8_t flags = 0;
    if (preset.chorusEnabled)     flags |= 0x01;
    if (preset.portamentoEnabled) flags |= 0x02;
    if (preset.legatoEnabled)     flags |= 0x04;
    if (isFm)                     flags |= 0x08; // Bit 3: FM Engine Mode (preserves FM mode in User slots)
    // Bits 4..7: instrument voicing (factory model index + 1; 0 = none / legacy patch), so a user
    // patch cloned from e.g. the Minimoog keeps its ladder filter, key tracking and velocity response
    // The nibble holds the v1-era models (indices 0..14) only; later models are not representable in v1
    const SynthModel voicing = resolveVoicingModel(preset);
    if (static_cast<uint8_t>(voicing) < kV1FactoryModels) flags |= static_cast<uint8_t>((static_cast<uint8_t>(voicing) + 1) << 4);
    outRaw.flags = flags;

    if (isFm) {
        outRaw.cutoffHz = static_cast<uint16_t>(std::round(std::clamp(preset.modDepth / 12.0f, 0.0f, 1.0f) * 65535.0f));
        outRaw.resonanceQ = static_cast<uint16_t>(std::round(std::clamp((preset.modRatio - 0.5f) / 13.5f, 0.0f, 1.0f) * 65535.0f));
        outRaw.subOscLevel = static_cast<uint16_t>(std::round(std::clamp(preset.modFeedback / 1.5f, 0.0f, 1.0f) * 65535.0f));
        outRaw.filterEnvAmount = static_cast<uint16_t>(std::round(std::clamp(preset.tremoloDepth, 0.0f, 1.0f) * 65535.0f));
        outRaw.detuneCents = 0;
        outRaw.drive = static_cast<uint16_t>(std::round(std::clamp((preset.drive - 1.0f) / 2.5f, 0.0f, 1.0f) * 65535.0f));
    } else {
        // Log-scaled Cutoff: 20 Hz .. 20000 Hz
        float normCutoff = (std::log10(std::clamp(preset.cutoffHz, 20.0f, 20000.0f)) - std::log10(20.0f)) /
                           (std::log10(20000.0f) - std::log10(20.0f));
        outRaw.cutoffHz = static_cast<uint16_t>(std::round(std::clamp(normCutoff, 0.0f, 1.0f) * 65535.0f));

        // Resonance Q: 0.5 .. 25.0
        float normQ = (std::clamp(preset.resonanceQ, 0.5f, 25.0f) - 0.5f) / (25.0f - 0.5f);
        outRaw.resonanceQ = static_cast<uint16_t>(std::round(std::clamp(normQ, 0.0f, 1.0f) * 65535.0f));

        outRaw.subOscLevel = static_cast<uint16_t>(std::round(std::clamp(preset.subOscLevel, 0.0f, 1.0f) * 65535.0f));
        outRaw.detuneCents = static_cast<uint16_t>(std::round(std::clamp(preset.detuneCents / 50.0f, 0.0f, 1.0f) * 65535.0f));
        outRaw.drive = static_cast<uint16_t>(std::round(std::clamp((preset.drive - 1.0f) / 2.5f, 0.0f, 1.0f) * 65535.0f));
        outRaw.filterEnvAmount = static_cast<uint16_t>(std::round(std::clamp(preset.filterEnvAmount, 0.0f, 1.0f) * 65535.0f));
    }

    // Amp ADSR
    float normAmpA = (std::log10(std::clamp(preset.ampAttackMs, 1.0f, 4000.0f)) - std::log10(1.0f)) /
                     (std::log10(4000.0f) - std::log10(1.0f));
    outRaw.ampAttackMs = static_cast<uint16_t>(std::round(std::clamp(normAmpA, 0.0f, 1.0f) * 65535.0f));

    float normAmpD = (std::log10(std::clamp(preset.ampDecayMs, 5.0f, 5000.0f)) - std::log10(5.0f)) /
                     (std::log10(5000.0f) - std::log10(5.0f));
    outRaw.ampDecayMs = static_cast<uint16_t>(std::round(std::clamp(normAmpD, 0.0f, 1.0f) * 65535.0f));

    outRaw.ampSustain = static_cast<uint16_t>(std::round(std::clamp(preset.ampSustain, 0.0f, 1.0f) * 65535.0f));

    float normAmpR = (std::log10(std::clamp(preset.ampReleaseMs, 5.0f, 5000.0f)) - std::log10(5.0f)) /
                     (std::log10(5000.0f) - std::log10(5.0f));
    outRaw.ampReleaseMs = static_cast<uint16_t>(std::round(std::clamp(normAmpR, 0.0f, 1.0f) * 65535.0f));

    // Filter ADSR
    float normFiltA = (std::log10(std::clamp(preset.filterAttackMs, 1.0f, 4000.0f)) - std::log10(1.0f)) /
                      (std::log10(4000.0f) - std::log10(1.0f));
    outRaw.filterAttackMs = static_cast<uint16_t>(std::round(std::clamp(normFiltA, 0.0f, 1.0f) * 65535.0f));

    float normFiltD = (std::log10(std::clamp(preset.filterDecayMs, 5.0f, 5000.0f)) - std::log10(5.0f)) /
                      (std::log10(5000.0f) - std::log10(5.0f));
    outRaw.filterDecayMs = static_cast<uint16_t>(std::round(std::clamp(normFiltD, 0.0f, 1.0f) * 65535.0f));

    outRaw.filterSustain = static_cast<uint16_t>(std::round(std::clamp(preset.filterSustain, 0.0f, 1.0f) * 65535.0f));

    float normFiltR = (std::log10(std::clamp(preset.filterReleaseMs, 5.0f, 5000.0f)) - std::log10(5.0f)) /
                      (std::log10(5000.0f) - std::log10(5.0f));
    outRaw.filterReleaseMs = static_cast<uint16_t>(std::round(std::clamp(normFiltR, 0.0f, 1.0f) * 65535.0f));

    // Chorus
    outRaw.chorusRate = static_cast<uint8_t>(std::round(std::clamp((preset.chorusRateHz - 0.1f) / 4.9f, 0.0f, 1.0f) * 255.0f));
    outRaw.chorusDepth = static_cast<uint8_t>(std::round(std::clamp((preset.chorusDepthMs - 0.5f) / 4.5f, 0.0f, 1.0f) * 255.0f));
    outRaw.chorusMix = static_cast<uint8_t>(std::round(std::clamp(preset.chorusMix, 0.0f, 1.0f) * 255.0f));
    outRaw.glideTime = static_cast<uint8_t>(std::round(std::clamp(preset.glideTimeMs / 1000.0f, 0.0f, 1.0f) * 255.0f));
}

void SysExProtocol::rawPatchToPreset(const RawPatchData& raw, Preset& outPreset, char* nameStorage) {
    if (nameStorage) {
        std::strncpy(nameStorage, raw.name, sizeof(raw.name) - 1);
        nameStorage[sizeof(raw.name) - 1] = '\0';
        outPreset.name = nameStorage;
    } else {
        static char localName[16];
        std::strncpy(localName, raw.name, sizeof(raw.name) - 1);
        localName[sizeof(raw.name) - 1] = '\0';
        outPreset.name = localName;
    }

    outPreset.description = "User or SysEx loaded patch";
    // v1 blobs were written when models 15..31 were the user slots: only 0..14 name a factory model
    outPreset.model = (raw.model < kV1FactoryModels) ? static_cast<SynthModel>(raw.model) : SynthModel::User;
    outPreset.waveform = (raw.waveform < static_cast<uint8_t>(Waveform::Count)) ?
                         static_cast<Waveform>(raw.waveform) : Waveform::Sawtooth;

    outPreset.voiceMode = (raw.voiceMode < 2) ?
                          static_cast<VoiceMode>(raw.voiceMode) : VoiceMode::Polyphonic;
    outPreset.chorusEnabled = (raw.flags & 0x01) != 0;
    outPreset.portamentoEnabled = (raw.flags & 0x02) != 0;
    outPreset.legatoEnabled = (raw.flags & 0x04) != 0;
    const uint8_t voicingField = static_cast<uint8_t>(raw.flags >> 4);
    outPreset.voicing = (voicingField >= 1 && voicingField <= kV1FactoryModels)
                        ? static_cast<SynthModel>(voicingField - 1) : SynthModel::Count;

    if (isFmModel(outPreset.model) || (raw.flags & 0x08) != 0) {
        outPreset.engineMode = EngineMode::FM;
        float normDepth = static_cast<float>(raw.cutoffHz) / 65535.0f;
        outPreset.modDepth = normDepth * 12.0f;
        float normRatio = static_cast<float>(raw.resonanceQ) / 65535.0f;
        outPreset.modRatio = 0.5f + normRatio * 13.5f;
        float normFb = static_cast<float>(raw.subOscLevel) / 65535.0f;
        outPreset.modFeedback = normFb * 1.5f;
        float normTrem = static_cast<float>(raw.filterEnvAmount) / 65535.0f;
        outPreset.tremoloDepth = normTrem;
        outPreset.tremoloRateHz = 4.5f;
        outPreset.cutoffHz = 2800.0f;
        outPreset.resonanceQ = 1.0f;
        outPreset.subOscLevel = 0.0f;
        outPreset.detuneCents = 0.0f;
        outPreset.drive = 1.0f + (static_cast<float>(raw.drive) / 65535.0f) * 2.5f;
        outPreset.filterEnvAmount = 0.5f;
    } else {
        outPreset.engineMode = EngineMode::Subtractive;
        float normCutoff = static_cast<float>(raw.cutoffHz) / 65535.0f;
        outPreset.cutoffHz = 20.0f * std::pow(20000.0f / 20.0f, normCutoff);

        float normQ = static_cast<float>(raw.resonanceQ) / 65535.0f;
        outPreset.resonanceQ = 0.5f + normQ * (25.0f - 0.5f);

        outPreset.subOscLevel = static_cast<float>(raw.subOscLevel) / 65535.0f;
        outPreset.detuneCents = (static_cast<float>(raw.detuneCents) / 65535.0f) * 50.0f;
        outPreset.drive = 1.0f + (static_cast<float>(raw.drive) / 65535.0f) * 2.5f;
        outPreset.filterEnvAmount = static_cast<float>(raw.filterEnvAmount) / 65535.0f;
    }

    // Amp ADSR
    float normAmpA = static_cast<float>(raw.ampAttackMs) / 65535.0f;
    outPreset.ampAttackMs = 1.0f * std::pow(4000.0f / 1.0f, normAmpA);

    float normAmpD = static_cast<float>(raw.ampDecayMs) / 65535.0f;
    outPreset.ampDecayMs = 5.0f * std::pow(5000.0f / 5.0f, normAmpD);

    outPreset.ampSustain = static_cast<float>(raw.ampSustain) / 65535.0f;

    float normAmpR = static_cast<float>(raw.ampReleaseMs) / 65535.0f;
    outPreset.ampReleaseMs = 5.0f * std::pow(5000.0f / 5.0f, normAmpR);

    // Filter ADSR
    float normFiltA = static_cast<float>(raw.filterAttackMs) / 65535.0f;
    outPreset.filterAttackMs = 1.0f * std::pow(4000.0f / 1.0f, normFiltA);

    float normFiltD = static_cast<float>(raw.filterDecayMs) / 65535.0f;
    outPreset.filterDecayMs = 5.0f * std::pow(5000.0f / 5.0f, normFiltD);

    outPreset.filterSustain = static_cast<float>(raw.filterSustain) / 65535.0f;

    float normFiltR = static_cast<float>(raw.filterReleaseMs) / 65535.0f;
    outPreset.filterReleaseMs = 5.0f * std::pow(5000.0f / 5.0f, normFiltR);

    // Chorus
    outPreset.chorusRateHz = 0.1f + (static_cast<float>(raw.chorusRate) / 255.0f) * 4.9f;
    outPreset.chorusDepthMs = 0.5f + (static_cast<float>(raw.chorusDepth) / 255.0f) * 4.5f;
    outPreset.chorusMix = static_cast<float>(raw.chorusMix) / 255.0f;
    outPreset.glideTimeMs = (static_cast<float>(raw.glideTime) / 255.0f) * 1000.0f;

    // Bank A and Bank B standard mapping
    outPreset.bankA = {
        {"Cutoff", "Resonnc", "SubOsc", "Chorus", "Attack", "Decay", "Sustain", "Release"},
        {74, 71, 77, 78, 73, 75, 79, 72},
        {75, 40, 32, 0,  20, 80, 89, 75}
    };
    outPreset.bankB = {
        {"FiltEnv", "F-Attack", "F-Decay", "F-Sust", "F-Rel", "ChoRate", "ChoDepth", "Master"},
        {76, 80, 81, 82, 83, 84, 85, 7},
        {64, 40, 85, 38, 75, 30, 60, 100}
    };
}

// -------------------------------------------------------------------------------------------------
// Patch-scope parameter table and PatchDumpV2 (ADR-0006)
// -------------------------------------------------------------------------------------------------

#define FLOAT_PARAM(name, curve, lo, hi, member)                                   \
    {ParamId::name, ParamCurve::curve, lo, hi,                                     \
     [](const Preset& p) -> float { return p.member; },                            \
     [](Preset& p, float v) { p.member = v; }}
#define BOOL_PARAM(name, member)                                                   \
    {ParamId::name, ParamCurve::Bool, 0.0f, 1.0f,                                  \
     [](const Preset& p) -> float { return p.member ? 1.0f : 0.0f; },              \
     [](Preset& p, float v) { p.member = v >= 0.5f; }}
#define DRAWBAR_PARAM(i)                                                           \
    {static_cast<ParamId>(static_cast<uint8_t>(ParamId::Drawbar1) + (i)), ParamCurve::Linear, 0.0f, 8.0f, \
     [](const Preset& p) -> float { return p.timbre.drawbars[i]; },                \
     [](Preset& p, float v) { p.timbre.drawbars[i] = v; }}
#define ENUM_PARAM(name, type, count, member)                                      \
    {ParamId::name, ParamCurve::Enum, 0.0f, static_cast<float>((count) - 1),       \
     [](const Preset& p) -> float { return static_cast<float>(p.member); },        \
     [](Preset& p, float v) { p.member = static_cast<type>(static_cast<uint8_t>(v)); }}

static const ParamDef PATCH_PARAMS[] = {
    FLOAT_PARAM(Cutoff,          Log,    20.0f, 20000.0f, cutoffHz),
    FLOAT_PARAM(Resonance,       Linear, 0.5f,  25.0f,    resonanceQ),
    ENUM_PARAM (Waveform,        Waveform, static_cast<uint8_t>(Waveform::Count), waveform),
    FLOAT_PARAM(SubOscLevel,     Linear, 0.0f,  1.0f,     subOscLevel),
    FLOAT_PARAM(DetuneCents,     Linear, 0.0f,  50.0f,    detuneCents),
    FLOAT_PARAM(Drive,           Linear, 1.0f,  3.5f,     drive),
    FLOAT_PARAM(FilterEnvAmount, Linear, 0.0f,  1.0f,     filterEnvAmount),
    // Engine and voicing are read resolved, as the engine applies them: factory presets carry them in `model`
    {ParamId::EngineMode, ParamCurve::Enum, 0.0f, static_cast<float>(static_cast<uint8_t>(EngineMode::Count) - 1),
     [](const Preset& p) -> float { return static_cast<float>(static_cast<uint8_t>(presetEngineMode(p))); },
     [](Preset& p, float v) { p.engineMode = static_cast<EngineMode>(static_cast<uint8_t>(v)); }},
    {ParamId::Voicing, ParamCurve::Enum, 0.0f, static_cast<float>(FACTORY_PRESET_COUNT),
     [](const Preset& p) -> float {
         const SynthModel v = resolveVoicingModel(p);
         return isFactoryModel(v) ? static_cast<float>(static_cast<uint8_t>(v) + 1) : 0.0f;
     },
     [](Preset& p, float v) {
         const uint8_t idx = static_cast<uint8_t>(v);
         p.voicing = idx == 0 ? SynthModel::Count : static_cast<SynthModel>(idx - 1);
     }},

    FLOAT_PARAM(AmpAttack,       Log,    1.0f,  4000.0f,  ampAttackMs),
    FLOAT_PARAM(AmpDecay,        Log,    5.0f,  5000.0f,  ampDecayMs),
    FLOAT_PARAM(AmpSustain,      Linear, 0.0f,  1.0f,     ampSustain),
    FLOAT_PARAM(AmpRelease,      Log,    5.0f,  5000.0f,  ampReleaseMs),
    FLOAT_PARAM(FilterAttack,    Log,    1.0f,  4000.0f,  filterAttackMs),
    FLOAT_PARAM(FilterDecay,     Log,    5.0f,  5000.0f,  filterDecayMs),
    FLOAT_PARAM(FilterSustain,   Linear, 0.0f,  1.0f,     filterSustain),
    FLOAT_PARAM(FilterRelease,   Log,    5.0f,  5000.0f,  filterReleaseMs),

    BOOL_PARAM (ChorusEnabled,   chorusEnabled),
    FLOAT_PARAM(ChorusRate,      Linear, 0.1f,  5.0f,     chorusRateHz),
    FLOAT_PARAM(ChorusDepth,     Linear, 0.5f,  5.0f,     chorusDepthMs),
    FLOAT_PARAM(ChorusMix,       Linear, 0.0f,  1.0f,     chorusMix),

    ENUM_PARAM (VoiceMode,       VoiceMode, 2, voiceMode),
    FLOAT_PARAM(GlideTime,       Linear, 0.0f,  1000.0f,  glideTimeMs),
    BOOL_PARAM (PortamentoEnabled, portamentoEnabled),
    BOOL_PARAM (LegatoEnabled,   legatoEnabled),

    FLOAT_PARAM(ModRatio,        Linear, 0.5f,  14.0f,    modRatio),
    FLOAT_PARAM(ModDepth,        Linear, 0.0f,  12.0f,    modDepth),
    FLOAT_PARAM(ModFeedback,     Linear, 0.0f,  1.5f,     modFeedback),
    FLOAT_PARAM(TremoloDepth,    Linear, 0.0f,  1.0f,     tremoloDepth),
    FLOAT_PARAM(TremoloRate,     Linear, 0.1f,  10.0f,    tremoloRateHz),

    ENUM_PARAM (WtTable,         uint8_t, WAVETABLE_COUNT, timbre.wtTable),
    FLOAT_PARAM(WtPosition,      Linear, 0.0f,  1.0f,     timbre.wtPosition),
    FLOAT_PARAM(WtEnvAmount,     Linear, 0.0f,  1.0f,     timbre.wtEnvAmount),
    ENUM_PARAM (PdWave,          PdWave, static_cast<uint8_t>(PdWave::Count), timbre.pdWave),
    FLOAT_PARAM(PdDcw,           Linear, 0.0f,  1.0f,     timbre.pdDcw),
    FLOAT_PARAM(PluckDecay,      Log,    100.0f, 10000.0f, timbre.pluckDecayMs),
    FLOAT_PARAM(PluckBright,     Linear, 0.0f,  1.0f,     timbre.pluckBright),
    FLOAT_PARAM(PluckPick,       Linear, 0.0f,  1.0f,     timbre.pluckPick),
    FLOAT_PARAM(PercLevel,       Linear, 0.0f,  1.0f,     timbre.percLevel),
    {ParamId::PercHarmonic, ParamCurve::Enum, 0.0f, 1.0f,
     [](const Preset& p) -> float { return p.timbre.percHarmonic ? 1.0f : 0.0f; },
     [](Preset& p, float v) { p.timbre.percHarmonic = v >= 0.5f ? 1 : 0; }},
    FLOAT_PARAM(KeyClick,        Linear, 0.0f,  1.0f,     timbre.keyClick),
    DRAWBAR_PARAM(0), DRAWBAR_PARAM(1), DRAWBAR_PARAM(2), DRAWBAR_PARAM(3), DRAWBAR_PARAM(4),
    DRAWBAR_PARAM(5), DRAWBAR_PARAM(6), DRAWBAR_PARAM(7), DRAWBAR_PARAM(8),
};

#undef FLOAT_PARAM
#undef BOOL_PARAM
#undef ENUM_PARAM
#undef DRAWBAR_PARAM

const ParamDef* SysExProtocol::patchParams(size_t& count) {
    count = sizeof(PATCH_PARAMS) / sizeof(PATCH_PARAMS[0]);
    return PATCH_PARAMS;
}

const ParamDef* SysExProtocol::findPatchParam(ParamId id) {
    for (const ParamDef& def : PATCH_PARAMS) {
        if (def.id == id) return &def;
    }
    return nullptr;
}

uint16_t SysExProtocol::paramToValue14(const ParamDef& def, float value) {
    float norm = 0.0f;
    switch (def.curve) {
        case ParamCurve::Enum:
            return static_cast<uint16_t>(std::lround(std::clamp(value, def.lo, def.hi)));
        case ParamCurve::Bool:
            return value >= 0.5f ? 1 : 0;
        case ParamCurve::Log:
            norm = std::log(std::clamp(value, def.lo, def.hi) / def.lo) / std::log(def.hi / def.lo);
            break;
        case ParamCurve::Linear:
            norm = (value - def.lo) / (def.hi - def.lo);
            break;
    }
    return static_cast<uint16_t>(std::lround(std::clamp(norm, 0.0f, 1.0f) * 16383.0f));
}

float SysExProtocol::value14ToParam(const ParamDef& def, uint16_t value14) {
    value14 &= 0x3FFF;
    const float norm = static_cast<float>(value14) / 16383.0f;
    switch (def.curve) {
        case ParamCurve::Enum:
            if (def.hi <= 1.0f) return value14 > 0 ? 1.0f : 0.0f; // On/off enums (VoiceMode, EngineMode): any nonzero is 1
            if (value14 <= def.hi) return static_cast<float>(value14);
            return std::min(def.hi, std::floor(norm * (def.hi + 1.0f)));
        case ParamCurve::Bool:
            return value14 > 0 ? 1.0f : 0.0f;
        case ParamCurve::Log:
            return def.lo * std::pow(def.hi / def.lo, norm);
        case ParamCurve::Linear:
            break;
    }
    return def.lo + norm * (def.hi - def.lo);
}

size_t SysExProtocol::encodePatchBody(const Preset& preset, uint8_t* outBody, size_t maxLen) {
    size_t nameLen = 0;
    if (preset.name) {
        while (nameLen < PATCH_NAME_MAX && preset.name[nameLen] != '\0') ++nameLen;
    }
    const size_t needed = 1 + nameLen + 3 * (sizeof(PATCH_PARAMS) / sizeof(PATCH_PARAMS[0]));
    if (!outBody || maxLen < needed) return 0;

    size_t pos = 0;
    outBody[pos++] = static_cast<uint8_t>(nameLen);
    for (size_t i = 0; i < nameLen; ++i) {
        const char c = preset.name[i];
        outBody[pos++] = (c >= 0x20 && c <= 0x7E) ? static_cast<uint8_t>(c) : '?';
    }
    for (const ParamDef& def : PATCH_PARAMS) {
        const uint16_t v = paramToValue14(def, def.get(preset));
        outBody[pos++] = static_cast<uint8_t>(def.id);
        outBody[pos++] = static_cast<uint8_t>((v >> 7) & 0x7F);
        outBody[pos++] = static_cast<uint8_t>(v & 0x7F);
    }
    return pos;
}

bool SysExProtocol::isValidPatchBody(const uint8_t* body, size_t len) {
    if (!body || len < 1 || len > MAX_PATCH_BODY_SIZE) return false;
    const size_t nameLen = body[0];
    if (nameLen > PATCH_NAME_MAX || 1 + nameLen > len) return false;
    for (size_t i = 1; i <= nameLen; ++i) {
        if (body[i] < 0x20 || body[i] > 0x7E) return false;
    }
    size_t pos = 1 + nameLen;
    while (pos < len) {
        const size_t entryLen = (body[pos] == PARAM_ID_EXTENDED) ? 5 : 3;
        if (pos + entryLen > len) return false;
        for (size_t i = 0; i < entryLen; ++i) {
            if (body[pos + i] & 0x80) return false;
        }
        pos += entryLen;
    }
    return true;
}

bool SysExProtocol::decodePatchBody(const uint8_t* body, size_t len, Preset& outPreset, char* nameStorage) {
    if (!isValidPatchBody(body, len) || !nameStorage) return false;

    const size_t nameLen = body[0];
    std::memcpy(nameStorage, &body[1], nameLen);
    nameStorage[nameLen] = '\0';
    outPreset.name = nameStorage;

    size_t pos = 1 + nameLen;
    while (pos < len) {
        if (body[pos] == PARAM_ID_EXTENDED) {
            pos += 5; // No extended IDs defined yet: skip
            continue;
        }
        const ParamDef* def = findPatchParam(static_cast<ParamId>(body[pos]));
        const uint16_t value14 = static_cast<uint16_t>((body[pos + 1] << 7) | body[pos + 2]);
        if (def) def->set(outPreset, value14ToParam(*def, value14)); // Unknown or global IDs are skipped
        pos += 3;
    }
    return true;
}

size_t SysExProtocol::encodePatchDumpV2(uint8_t slot, const Preset& preset, uint8_t* outBuffer, size_t maxLen) {
    if (!outBuffer || maxLen < PATCH_V2_FRAMING_SIZE + 1) return 0;
    const size_t bodyLen = encodePatchBody(preset, &outBuffer[5], maxLen - PATCH_V2_FRAMING_SIZE);
    if (bodyLen == 0) return 0;

    outBuffer[0] = SYSEX_START;
    outBuffer[1] = SYSEX_MANUFACTURER_ID;
    outBuffer[2] = SYSEX_FAMILY_ID;
    outBuffer[3] = static_cast<uint8_t>(SysExCommand::PatchDumpV2);
    outBuffer[4] = slot & 0x7F;
    const size_t checksumPos = 5 + bodyLen;
    outBuffer[checksumPos] = computeChecksum(&outBuffer[4], 1 + bodyLen);
    outBuffer[checksumPos + 1] = SYSEX_END;
    return checksumPos + 2;
}

} // namespace Synth
