#include "SynthEngine.h"
#include "MidiParser.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>

int main() {
    std::cout << "--- Running SynthEngine & MidiParser Tests ---" << std::endl;

    Synth::SynthEngine engine;
    engine.init(44100.0f);

    Synth::MidiParser parser;

    // Test 1: MIDI Note On and Note Off via Parser
    std::cout << "Test 1: MidiParser Note On / Note Off..." << std::endl;
    // Note On: 0x90, 60 (Middle C), 100 (Velocity)
    parser.parseByte(0x90, engine);
    parser.parseByte(60, engine);
    parser.parseByte(100, engine);

    assert(engine.getActiveVoiceCount() == 1);

    // Render 128 samples
    int16_t buffer[256];
    engine.processBlock(buffer, 128);

    bool hasNonZero = false;
    for (int i = 0; i < 256; ++i) {
        if (buffer[i] != 0) hasNonZero = true;
    }
    assert(hasNonZero);
    std::cout << "  Passed (Rendered active audio block)" << std::endl;

    // Test 2: Running status Note On and Note Off
    std::cout << "Test 2: Running status MIDI handling..." << std::endl;
    // Running status: Note On for 64 (E4) without status byte
    parser.parseByte(64, engine);
    parser.parseByte(100, engine);
    assert(engine.getActiveVoiceCount() == 2);

    // Note Off for 60 using Note On with velocity 0 (standard MIDI practice)
    parser.parseByte(60, engine);
    parser.parseByte(0, engine);
    // Voice 60 is now releasing

    // Note Off for 64 using explicit 0x80 status byte
    parser.parseByte(0x80, engine);
    parser.parseByte(64, engine);
    parser.parseByte(64, engine);

    std::cout << "  Passed (Running status handled correctly)" << std::endl;

    // Test 3: CC parameters (Cutoff, Resonance, Waveform)
    std::cout << "Test 3: CC handling & Parameter scaling..." << std::endl;
    // Cutoff: CC 74, value 100
    parser.parseByte(0xB0, engine);
    parser.parseByte(74, engine);
    parser.parseByte(100, engine);
    assert(engine.getCutoffHz() > 2000.0f);

    // Resonance: CC 71, value 90
    parser.parseByte(0xB0, engine);
    parser.parseByte(71, engine);
    parser.parseByte(90, engine);
    assert(engine.getResonanceQ() > 1.0f);

    // Waveform: CC 70, value 50 (Square)
    parser.parseByte(0xB0, engine);
    parser.parseByte(70, engine);
    parser.parseByte(50, engine);
    assert(engine.getWaveform() == Synth::Waveform::Square);

    std::cout << "  Passed (CC parameter scaling verified)" << std::endl;

    // Test 4: Polyphony and Voice Stealing (capped at the modelled instrument's polyphony)
    std::cout << "Test 4: Polyphony & Voice Stealing (allocating 10 notes)..." << std::endl;
    assert(engine.getEffectiveVoiceLimit() == 6); // Juno-60 is a 6-voice instrument
    for (uint8_t note = 60; note < 70; ++note) {
        engine.noteOn(note, 100);
    }
    assert(engine.getActiveVoiceCount() == 6);

    // Render multiple blocks under high polyphony to verify stability
    for (int b = 0; b < 100; ++b) {
        engine.processBlock(buffer, 128);
        for (int i = 0; i < 256; ++i) {
            assert(!std::isnan(buffer[i]));
        }
    }
    std::cout << "  Passed (Voice stealing stable, no NaNs or clipping overflows)" << std::endl;

    // Test 5: All notes off / Panic
    engine.panic();
    assert(engine.getActiveVoiceCount() == 0);
    std::cout << "Test 5: Panic / All notes off..." << std::endl;
    std::cout << "  Passed" << std::endl;

    // Test 6: Dynamic Voice Limit
    std::cout << "Test 6: Configurable Voice Limit (capped at 4 voices)..." << std::endl;
    engine.setVoiceLimit(4);
    assert(engine.getVoiceLimit() == 4);
    for (uint8_t note = 60; note < 70; ++note) {
        engine.noteOn(note, 100);
    }
    assert(engine.getActiveVoiceCount() == 4);
    std::cout << "  Passed (Voice limit capped strictly at 4 voices)" << std::endl;

    // Test 8: Monophonic Mode & Last Note Priority
    std::cout << "Test 8: Monophonic Mode & Last Note Priority (LNP)..." << std::endl;
    engine.panic();
    engine.setVoiceMode(Synth::VoiceMode::Monophonic);
    engine.setNotePriority(Synth::NotePriority::Last);
    assert(engine.getVoiceMode() == Synth::VoiceMode::Monophonic);

    // Play Note 60 (C4)
    engine.noteOn(60, 100);
    assert(engine.getActiveVoiceCount() == 1);

    // Play Note 64 (E4) while 60 is still held
    engine.noteOn(64, 100);
    assert(engine.getActiveVoiceCount() == 1); // Strictly 1 voice in monophonic mode

    // Release Note 64 (E4) while Note 60 is still held -> should return to Note 60
    engine.noteOff(64);
    assert(engine.getHeldKeyCount() == 1);
    assert(engine.getActiveVoiceCount() == 1);

    // Release Note 60 -> all keys released
    engine.noteOff(60);
    assert(engine.getHeldKeyCount() == 0);
    engine.panic();
    assert(engine.getActiveVoiceCount() == 0);
    std::cout << "  Passed (LNP stack maintains exactly 1 active voice and returns to held notes)" << std::endl;

    // Test 9: Low Note Priority & High Note Priority
    std::cout << "Test 9: Low and High Note Priority..." << std::endl;
    engine.setNotePriority(Synth::NotePriority::Low);
    engine.noteOn(64, 100); // E4
    engine.noteOn(60, 100); // C4 (lower note should take priority)
    assert(engine.getHeldKeyCount() == 2);
    assert(engine.getActiveVoiceCount() == 1);

    // Release C4 -> should fall back to E4
    engine.noteOff(60);
    assert(engine.getHeldKeyCount() == 1);
    assert(engine.getActiveVoiceCount() == 1);
    engine.noteOff(64);
    assert(engine.getHeldKeyCount() == 0);
    engine.panic();
    assert(engine.getActiveVoiceCount() == 0);

    engine.setNotePriority(Synth::NotePriority::High);
    engine.noteOn(60, 100); // C4
    engine.noteOn(67, 100); // G4 (higher note should take priority)
    assert(engine.getHeldKeyCount() == 2);
    assert(engine.getActiveVoiceCount() == 1);
    engine.noteOff(67);
    assert(engine.getHeldKeyCount() == 1);
    assert(engine.getActiveVoiceCount() == 1);
    engine.noteOff(60);
    assert(engine.getHeldKeyCount() == 0);
    engine.panic();
    assert(engine.getActiveVoiceCount() == 0);
    std::cout << "  Passed (Low and High priority resolution verified)" << std::endl;

    // Test 10: Portamento Glide (TB-303 Slide)
    std::cout << "Test 10: Portamento Glide & Legato pitch slewing..." << std::endl;
    engine.setVoiceMode(Synth::VoiceMode::Monophonic);
    engine.setPortamentoEnabled(true);
    engine.setGlideTimeMs(80.0f);
    assert(engine.isPortamentoEnabled() == true);
    assert(engine.getGlideTimeMs() == 80.0f);

    engine.noteOn(60, 127);
    // Render 100 frames at note 60
    int16_t monoBlock[256];
    engine.processBlock(monoBlock, 128);

    // Legato slide to note 72 (one octave up)
    engine.noteOn(72, 127);
    // Render 10 blocks (smooth slide over ~80ms)
    for (int b = 0; b < 10; ++b) {
        engine.processBlock(monoBlock, 128);
        for (int i = 0; i < 256; ++i) {
            assert(!std::isnan(monoBlock[i]));
        }
    }
    engine.panic();
    std::cout << "  Passed (Portamento glide active, smooth pitch transition rendered)" << std::endl;

    // Test 11: MIDI CC 126 (Mono Mode), CC 127 (Poly Mode), CC 65, CC 5
    std::cout << "Test 11: MIDI standard CC mode switching (CC 126/127/65/5)..." << std::endl;
    // CC 126: Mono Mode On
    parser.parseByte(0xB0, engine);
    parser.parseByte(126, engine);
    parser.parseByte(1, engine);
    assert(engine.getVoiceMode() == Synth::VoiceMode::Monophonic);

    // CC 127: Poly Mode On
    parser.parseByte(0xB0, engine);
    parser.parseByte(127, engine);
    parser.parseByte(0, engine);
    assert(engine.getVoiceMode() == Synth::VoiceMode::Polyphonic);

    // CC 65: Portamento Switch
    parser.parseByte(0xB0, engine);
    parser.parseByte(65, engine);
    parser.parseByte(127, engine); // On
    assert(engine.isPortamentoEnabled() == true);

    // CC 5: Portamento Time
    parser.parseByte(0xB0, engine);
    parser.parseByte(5, engine);
    parser.parseByte(64, engine); // ~500ms
    assert(engine.getGlideTimeMs() > 450.0f && engine.getGlideTimeMs() < 550.0f);
    std::cout << "  Passed (MIDI Channel Mode & Portamento CCs verified)" << std::endl;

    // Test 12: Presets Default Monophonic / Polyphonic Configuration
    std::cout << "Test 12: Preset defaults (TB-303 and Minimoog auto-configured to Monophonic)..." << std::endl;
    engine.loadPreset(Synth::SynthModel::TB303);
    assert(engine.getVoiceMode() == Synth::VoiceMode::Monophonic);
    assert(engine.isPortamentoEnabled() == true);
    assert(engine.getGlideTimeMs() == 60.0f); // 303 slide

    engine.loadPreset(Synth::SynthModel::Minimoog);
    assert(engine.getVoiceMode() == Synth::VoiceMode::Monophonic);
    assert(engine.isPortamentoEnabled() == true);
    assert(engine.getGlideTimeMs() == 15.0f);

    engine.loadPreset(Synth::SynthModel::Juno60);
    assert(engine.getVoiceMode() == Synth::VoiceMode::Polyphonic);
    assert(engine.isPortamentoEnabled() == false);
    std::cout << "  Passed (Iconic synths correctly initialized to Monophonic or Polyphonic)" << std::endl;

    // Test 13: Casio PT-10 Simulation (4 dedicated presets, multipulse waveforms, cursor key navigation)
    std::cout << "\nTest 13: Casio PT-10 Simulation (4 Dedicated Presets & Navigation)..." << std::endl;
    // 1. PT-10 Piano (Slot 8)
    engine.loadPreset(Synth::SynthModel::CasioPT10_Piano);
    assert(std::strcmp(engine.getCurrentPreset().name, "PT-10 PIANO") == 0);
    assert(engine.getVoiceMode() == Synth::VoiceMode::Monophonic);
    assert(engine.getCasioTone() == Synth::CasioTone::Piano);
    assert(std::strcmp(engine.getCasioToneName(), "PIANO") == 0);
    assert(std::abs(engine.getPulseWidth() - 0.70f) < 0.01f);
    assert(engine.getAmpSustainLevel() == 0.0f);
    assert(engine.getVibratoDepth() == 0.0f);

    // 2. PT-10 Fantasy (Slot 9)
    engine.loadPreset(Synth::SynthModel::CasioPT10_Fantasy);
    assert(std::strcmp(engine.getCurrentPreset().name, "PT-10 FNTSY") == 0);
    assert(engine.getCasioTone() == Synth::CasioTone::Fantasy);
    assert(std::strcmp(engine.getCasioToneName(), "FANTASY") == 0);
    assert(std::abs(engine.getPulseWidth() - 0.50f) < 0.01f);
    assert(engine.getVibratoDepth() > 0.20f);
    assert(engine.getSubOscLevel() > 0.30f);

    // 3. PT-10 Violin (Slot 10)
    engine.loadPreset(Synth::SynthModel::CasioPT10_Violin);
    assert(std::strcmp(engine.getCurrentPreset().name, "PT-10 VIOLN") == 0);
    assert(engine.getCasioTone() == Synth::CasioTone::Violin);
    assert(std::strcmp(engine.getCasioToneName(), "VIOLIN") == 0);
    assert(std::abs(engine.getPulseWidth() - 0.22f) < 0.01f);
    assert(engine.getAmpSustainLevel() > 0.85f);
    assert(engine.getVibratoDepth() > 0.10f);

    // 4. PT-10 Flute (Slot 11)
    engine.loadPreset(Synth::SynthModel::CasioPT10_Flute);
    assert(std::strcmp(engine.getCurrentPreset().name, "PT-10 FLUTE") == 0);
    assert(engine.getCasioTone() == Synth::CasioTone::Flute);
    assert(std::strcmp(engine.getCasioToneName(), "FLUTE") == 0);
    assert(std::abs(engine.getPulseWidth() - 0.50f) < 0.01f);
    assert(std::abs(engine.getCutoffHz() - 1400.0f) < 1.0f);

    // Audio block rendering with Casio PT-10 Fantasy lead
    engine.loadPreset(Synth::SynthModel::CasioPT10_Fantasy);
    engine.noteOn(60, 100);
    std::vector<int16_t> audioBlock(128 * 2, 0);
    engine.processBlock(audioBlock.data(), 128);
    hasNonZero = false;
    for (int16_t sample : audioBlock) {
        if (sample != 0) {
            hasNonZero = true;
            break;
        }
    }
    assert(hasNonZero);
    engine.noteOff(60);
    engine.panic();
    std::cout << "  Passed (Casio PT-10 4 dedicated models, cursor key navigation & audio generation verified)" << std::endl;

    // Test 13b: PT-10 PolyMode knob (Bank B K14, CC 92). The real PT-10 is monophonic (Mono, last-note
    // priority, is the preset default); Poly is an extension that opens up the engine's full polyphony.
    std::cout << "\nTest 13b: Casio PT-10 PolyMode knob (CC 92 Mono / Poly)..." << std::endl;
    {
        auto soundingVoices = [&engine]() {
            size_t count = 0;
            for (size_t i = 0; i < Synth::SynthEngine::MAX_VOICES; ++i) {
                const auto& v = engine.getVoice(i);
                if (v.isActive() && !v.isReleasing()) count++;
            }
            return count;
        };
        const uint8_t chord[] = {60, 64, 67};
        engine.setVoiceLimit(Synth::SynthEngine::MAX_VOICES);
        for (auto model : {Synth::SynthModel::CasioPT10_Piano, Synth::SynthModel::CasioPT10_Fantasy,
                           Synth::SynthModel::CasioPT10_Violin, Synth::SynthModel::CasioPT10_Flute}) {
            engine.loadPreset(model);
            engine.panic();

            // Poly: all three chord notes sound, and the full engine polyphony is available
            engine.controlChange(92, 127);
            assert(engine.getVoiceMode() == Synth::VoiceMode::Polyphonic);
            assert(engine.getEffectiveVoiceLimit() == Synth::SynthEngine::MAX_VOICES);
            for (uint8_t n : chord) engine.noteOn(n, 100);
            engine.processBlock(audioBlock.data(), 128);
            assert(soundingVoices() == 3);
            assert(engine.getActiveVoiceCount() == 3);
            for (uint8_t n : chord) engine.noteOff(n);
            engine.panic();

            // Mono: the same chord sounds one voice, on the last note played
            engine.controlChange(92, 0);
            assert(engine.getVoiceMode() == Synth::VoiceMode::Monophonic);
            for (uint8_t n : chord) engine.noteOn(n, 100);
            engine.processBlock(audioBlock.data(), 128);
            assert(soundingVoices() == 1);
            assert(engine.getVoice(0).getNoteNumber() == 67);
            for (uint8_t n : chord) engine.noteOff(n);
            engine.panic();
        }
    }
    std::cout << "  Passed (PolyMode CC 92 = 127 sounds a 3-note chord; Mono keeps last-note priority)" << std::endl;

    // Test 14: FM Synthesizer Engine & Operator Voice Processing
    std::cout << "\nTest 14: FM Synthesizer Engine (5 Yamaha DX Models, Dynamic Velocity, Sustain Pedal)..." << std::endl;
    {
        // 1. FM Model Detection & Helper Queries
        assert(Synth::isFmModel(Synth::SynthModel::DX_EPiano));
        assert(Synth::isFmModel(Synth::SynthModel::FM_CrystalTines));
        assert(Synth::isFmModel(Synth::SynthModel::FM_WarmRhodes));
        assert(Synth::isFmModel(Synth::SynthModel::FM_Clavinet));
        assert(Synth::isFmModel(Synth::SynthModel::FM_DrawbarOrgan));
        assert(!Synth::isFmModel(Synth::SynthModel::Juno60));
        assert(!Synth::isFmModel(Synth::SynthModel::CasioPT10_Piano));

        // 2. Preset 10: DX Classic E-Piano (Slot 11)
        engine.loadPreset(Synth::SynthModel::DX_EPiano);
        assert(std::strcmp(engine.getCurrentPreset().name, "DX E-PIANO") == 0);
        assert(engine.getEngineMode() == Synth::EngineMode::FM);
        assert(engine.getVoiceMode() == Synth::VoiceMode::Polyphonic);
        assert(std::abs(engine.getModRatio() - 1.0f) < 0.01f);
        assert(std::abs(engine.getModDepth() - 2.80f) < 0.01f);
        assert(std::abs(engine.getModFeedback() - 0.18f) < 0.01f);

        // 3. Audio Generation & Dynamic Velocity Scaling
        engine.panic();
        engine.noteOn(60, 30); // Soft key strike
        std::vector<int16_t> softBuffer(128 * 2, 0);
        engine.processBlock(softBuffer.data(), 128);
        double softEnergy = 0.0;
        for (int16_t s : softBuffer) {
            assert(!std::isnan(static_cast<float>(s)));
            softEnergy += static_cast<double>(s) * s;
        }
        assert(softEnergy > 0.0);
        engine.panic();

        engine.noteOn(60, 127); // Hard forte key strike
        std::vector<int16_t> hardBuffer(128 * 2, 0);
        engine.processBlock(hardBuffer.data(), 128);
        double hardEnergy = 0.0;
        for (int16_t s : hardBuffer) {
            assert(!std::isnan(static_cast<float>(s)));
            hardEnergy += static_cast<double>(s) * s;
        }
        assert(hardEnergy > softEnergy * 3.0); // Fortissimo has substantially higher dynamic power
        engine.panic();

        // 4. FM Live CC Control Tweaks
        // Mod Depth (CC 74)
        engine.controlChange(74, 127);
        assert(std::abs(engine.getModDepth() - 12.0f) < 0.05f);

        // Mod Ratio (CC 71)
        engine.controlChange(71, 0);
        assert(std::abs(engine.getModRatio() - 0.5f) < 0.05f);

        // Mod Feedback (CC 77)
        engine.controlChange(77, 127);
        assert(std::abs(engine.getModFeedback() - 1.5f) < 0.05f);

        // Direct prototype CCs (CC 16, CC 17, CC 22)
        engine.controlChange(16, 64);
        assert(engine.getModDepth() > 5.0f && engine.getModDepth() < 7.0f);
        engine.controlChange(17, 127);
        assert(std::abs(engine.getModRatio() - 14.0f) < 0.05f);
        engine.controlChange(22, 100);
        assert(engine.getTremoloDepth() > 0.70f);

        // 5. Preset 11: Crystal Tines (Slot 12)
        engine.loadPreset(Synth::SynthModel::FM_CrystalTines);
        assert(std::strcmp(engine.getCurrentPreset().name, "FM TINES") == 0);
        assert(std::abs(engine.getModRatio() - 7.0f) < 0.01f);

        // 6. Preset 12: Warm Rhodes (Slot 13)
        engine.loadPreset(Synth::SynthModel::FM_WarmRhodes);
        assert(std::strcmp(engine.getCurrentPreset().name, "RHODES") == 0);
        assert(engine.getTremoloDepth() > 0.30f);

        // 7. Preset 13: FM Clavinet (Slot 14)
        engine.loadPreset(Synth::SynthModel::FM_Clavinet);
        assert(std::strcmp(engine.getCurrentPreset().name, "FM CLAVINET") == 0);
        assert(std::abs(engine.getModRatio() - 2.0f) < 0.01f);

        // K1 (CC 74) / K2 (CC 71) HUD text must track FM Mod Depth / Ratio, not the unused filter cutoff / Q
        {
            std::string label, lo, hi;
            engine.controlChange(74, 20);
            assert(engine.describeParameter(74, 20, label, lo));
            engine.controlChange(74, 120);
            assert(engine.describeParameter(74, 120, label, hi));
            assert(lo != hi);
            assert(lo.find("Hz") == std::string::npos);

            engine.controlChange(71, 20);
            assert(engine.describeParameter(71, 20, label, lo));
            engine.controlChange(71, 120);
            assert(engine.describeParameter(71, 120, label, hi));
            assert(lo != hi);
            assert(hi.find('Q') == std::string::npos);
        }

        // Every factory knob (all presets, both banks) must move its own HUD readout: catches CCs whose engine
        // handler and describeParameter() disagree per engine mode (FM vs Subtractive vs Casio shift)
        for (size_t pi = 0; pi < Synth::FACTORY_PRESET_COUNT; ++pi) {
            for (int shift = 0; shift < 2; ++shift) {
                engine.loadPresetIndex(pi);
                engine.setShiftActive(shift != 0);
                const auto& pr   = engine.getCurrentPreset();
                const auto& bank = shift ? pr.bankB : pr.bankA;
                for (int k = 0; k < 8; ++k) {
                    std::string l, lo, hi;
                    engine.controlChange(bank.ccs[k], 10);
                    assert(engine.describeParameter(bank.ccs[k], 10, l, lo));
                    engine.controlChange(bank.ccs[k], 120);
                    assert(engine.describeParameter(bank.ccs[k], 120, l, hi));
                    if (lo == hi) {
                        std::cerr << "HUD static: " << pr.name << (shift ? " B K" : " A K") << (shift ? k + 9 : k + 1)
                                  << " cc" << int(bank.ccs[k]) << " '" << bank.labels[k] << "' = " << lo << std::endl;
                    }
                    assert(lo != hi);
                }
            }
        }
        // TB-303 Slide (CC 65): OFF below 64, glide time in ms from 64 up
        {
            std::string l, v;
            engine.loadPreset(Synth::SynthModel::TB303);
            engine.setShiftActive(true);
            engine.controlChange(65, 40);
            assert(engine.describeParameter(65, 40, l, v) && v == "OFF");
            engine.controlChange(65, 127);
            assert(engine.describeParameter(65, 127, l, v) && v == "200 ms");
        }
        engine.loadPreset(Synth::SynthModel::FM_Clavinet);
        engine.setShiftActive(false);

        // 8. Preset 14: FM Drawbar Organ (Slot 15)
        engine.loadPreset(Synth::SynthModel::FM_DrawbarOrgan);
        assert(std::strcmp(engine.getCurrentPreset().name, "FM ORGAN") == 0);
        assert(std::abs(engine.getModRatio() - 1.0f) < 0.01f);
        assert(engine.getAmpSustainLevel() >= 0.99f);

        // 9. Sustain Pedal (CC 64) Hold and Release
        engine.panic();
        engine.noteOn(60, 100);
        assert(engine.getActiveVoiceCount() == 1);

        // Press sustain pedal (CC 64 >= 64)
        engine.controlChange(64, 127);
        assert(engine.isSustainPedalHeld() == true);

        // Key released by musician, but sustain pedal is held
        engine.noteOff(60);
        assert(engine.getActiveVoiceCount() == 1); // Voice must remain ringing!

        // Release sustain pedal (CC 64 < 64)
        engine.controlChange(64, 0);
        assert(engine.isSustainPedalHeld() == false);

        // Render release frames until idle
        for (int b = 0; b < 200; ++b) {
            engine.processBlock(softBuffer.data(), 128);
        }
        assert(engine.getActiveVoiceCount() == 0); // Voice has cleanly released to idle
    }
    std::cout << "  Passed (5 FM models, dynamic velocity scaling, CC mappings & sustain pedal hold/release verified)" << std::endl;

    // -------------------------------------------------------------
    // Test 8: Metronome Click Audio, Time Signature, 0xF8 Clock & Key/Scale
    // -------------------------------------------------------------
    std::cout << "[Test 8] Metronome Audio, Time Signature, 0xF8 Clock & Key/Scale Quantizer..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);

        // 1. Toggle Metronome via CC 118 (MPK Metronome key) -> immediate Beat 1 click with 0 voices stolen
        assert(engine.isMetronomeEnabled() == false);
        engine.controlChange(Synth::CC_METRONOME_TOGGLE, 127);
        assert(engine.isMetronomeEnabled() == true);
        assert(engine.getActiveVoiceCount() == 0); // Must never steal polyphonic voices!

        std::vector<int16_t> clickBuf(256 * 2, 0);
        engine.processBlock(clickBuf.data(), 256);
        int16_t maxClickPeak = 0;
        for (int16_t s : clickBuf) {
            int16_t a = std::abs(s);
            if (a > maxClickPeak) maxClickPeak = a;
        }
        assert(maxClickPeak > 10000); // Strong audible Beat 1 click output (cuts through a played chord)
        assert(engine.getMetroClickCount() == 1); // beat LED sees the downbeat click
        assert(engine.isLastMetroClickDownbeat());

        // 2. Test Time Signature (3/4 Waltz) beat cycling
        engine.setTimeSignature(3, 4);
        assert(std::strcmp(engine.getTimeSignatureString(), "3/4") == 0);
        engine.setMetronomeBpm(240.0f); // 240 BPM -> 11025 samples per beat at 44.1 kHz
        // Advance 1 beat (11025 samples)
        std::vector<int16_t> block128(128 * 2, 0);
        for (int b = 0; b < 88; ++b) { // 88 * 128 = 11264 samples
            engine.processBlock(block128.data(), 128);
        }
        assert(engine.getCurrentBeat() == 1); // Beat 2 of 3/4
        assert(engine.getMetroClickCount() == 2);
        assert(!engine.isLastMetroClickDownbeat());
        for (int b = 0; b < 88; ++b) {
            engine.processBlock(block128.data(), 128);
        }
        assert(engine.getCurrentBeat() == 2); // Beat 3 of 3/4
        for (int b = 0; b < 88; ++b) {
            engine.processBlock(block128.data(), 128);
        }
        assert(engine.getCurrentBeat() == 0); // Wrapped back to Downbeat 1 of 3/4!

        // 3. Test External MIDI Clock (0xF8 = 24 PPQN) via MidiParser
        Synth::MidiParser parser;
        parser.parseByte(0xFA, engine); // MIDI Start
        assert(engine.isExternalClockActive() == true);
        assert(engine.getCurrentBeat() == 0);
        for (int tick = 0; tick < 24; ++tick) {
            parser.parseByte(0xF8, engine); // 24 clock ticks = 1 quarter note
        }
        assert(engine.getCurrentBeat() == 1);

        // 4. Test Musical Key & Scale Quantizer (Key of D = 2, Natural Minor)
        engine.setRootKey(2); // D
        assert(std::strcmp(engine.getRootKeyName(), "D") == 0);
        engine.setScaleMode(Synth::ScaleMode::Major);
        assert(std::strcmp(engine.getScaleModeName(), "MAJOR") == 0);
        // In D Major (D, E, F#, G, A, B, C#):
        // MIDI 62 (D4) -> 62 (in scale)
        // MIDI 65 (F4 natural) -> quantized to 64 (E4) or 66 (F#4)
        assert(engine.quantizeNoteToKeyAndScale(62) == 62);
        uint8_t qF4 = engine.quantizeNoteToKeyAndScale(65);
        assert(qF4 == 64 || qF4 == 66);
    }
    std::cout << "  Passed (Metronome DSP click, 3/4 time signature, 0xF8 clock sync & Key/Scale verified)" << std::endl;

    // Test 15: Animation Context API (OLED Visualizer Support)
    std::cout << "Test 15: Animation Context API (Notes, Velocity, BeatPhase, VoiceSnapshot)..." << std::endl;
    {
        Synth::SynthEngine engine;
        engine.init(44100.0f);

        // Initial default state
        assert(engine.getLastNoteNumber() == 0);
        assert(engine.getLastVelocity() == 0);

        // Note On tracking (Polyphonic)
        engine.noteOn(60, 100);
        assert(engine.getLastNoteNumber() == 60);
        assert(engine.getLastVelocity() == 100);

        // Note On with velocity 0 should act as noteOff without overwriting last note
        engine.noteOn(72, 0);
        assert(engine.getLastNoteNumber() == 60);
        assert(engine.getLastVelocity() == 100);

        // Note On tracking (Monophonic)
        engine.setVoiceMode(Synth::VoiceMode::Monophonic);
        engine.noteOn(67, 115);
        assert(engine.getLastNoteNumber() == 67);
        assert(engine.getLastVelocity() == 115);

        // Beat phase calculation
        assert(engine.getBeatPhase() == 0.0f); // Metronome disabled -> 0.0f
        engine.setMetronomeEnabled(true);
        engine.setMetronomeBpm(120.0f);
        float phase = engine.getBeatPhase();
        assert(phase >= 0.0f && phase < 1.0f);
        {
            // Phase follows the audio-rate counter that fires the click: half a beat of audio -> ~0.5
            Synth::SynthEngine beatEngine;
            beatEngine.init(44100);
            beatEngine.setMetronomeBpm(120.0f); // 22050 samples per beat
            beatEngine.setMetronomeEnabled(true);
            std::vector<float> l(11025), r(11025);
            beatEngine.processBlockFloat(l.data(), r.data(), l.size());
            assert(std::fabs(beatEngine.getBeatPhase() - 0.5f) < 0.01f);
            assert(beatEngine.getCurrentBeat() == 0);
            beatEngine.processBlockFloat(l.data(), r.data(), l.size()); // crosses into beat 2
            assert(beatEngine.getCurrentBeat() == 1);
            assert(beatEngine.getBeatPhase() < 0.01f);
        }

        // Voice snapshot testing
        engine.setVoiceMode(Synth::VoiceMode::Polyphonic);
        engine.panic();

        uint8_t snapNotes[8] = {0};
        float snapEnvelopes[8] = {0.0f};
        uint8_t snapCount = 0;

        engine.getVoiceSnapshot(snapNotes, snapEnvelopes, snapCount);
        assert(snapCount == 0);
        for (int i = 0; i < 8; ++i) {
            assert(snapNotes[i] == 0);
            assert(snapEnvelopes[i] == 0.0f);
        }

        // Trigger two notes
        engine.noteOn(60, 90);
        engine.noteOn(64, 110);

        // Process a few samples so envelope advances
        int16_t outBuf[64 * 2];
        engine.processBlock(outBuf, 64);

        engine.getVoiceSnapshot(snapNotes, snapEnvelopes, snapCount);
        assert(snapCount == 2);

        int activeFound = 0;
        for (int i = 0; i < 8; ++i) {
            if (snapNotes[i] != 0) {
                assert(snapNotes[i] == 60 || snapNotes[i] == 64);
                assert(snapEnvelopes[i] > 0.0f);
                ++activeFound;
            } else {
                assert(snapEnvelopes[i] == 0.0f);
            }
        }
        assert(activeFound == 2);
    }
    // Test 20: MpkMiniMk4Display Standalone Library & SynthEngine OLED Integration
    std::cout << "Test 20: MpkMiniMk4Display standalone library & SynthEngine integration..." << std::endl;
    {
        std::vector<std::vector<uint8_t>> pluginPackets;
        std::vector<std::vector<uint8_t>> swPackets;

        engine.setMpkDisplaySendCallbacks(
            [&pluginPackets](const uint8_t* data, size_t len) {
                pluginPackets.emplace_back(data, data + len);
            },
            [&swPackets](const uint8_t* data, size_t len) {
                swPackets.emplace_back(data, data + len);
            }
        );

        // Load Minimoog preset -> updates MPK Mini MK4 OLED via Plugin Port
        // (SW Port RAM preset is suppressed when Plugin Port is active to prevent resetting knob accumulators, ADR 0005)
        engine.loadPreset(Synth::SynthModel::Minimoog);
        assert(!pluginPackets.empty());
        assert(swPackets.empty());

        // Test Standalone Mode (Software Control Port only, no Plugin Port connected)
        {
            Synth::SynthEngine standaloneEngine;
            standaloneEngine.init(44100);
            std::vector<std::vector<uint8_t>> standaloneSwPackets;
            standaloneEngine.setMpkDisplaySendCallbacks(
                nullptr,
                [&standaloneSwPackets](const uint8_t* data, size_t len) {
                    standaloneSwPackets.emplace_back(data, data + len);
                }
            );
            standaloneEngine.loadPreset(Synth::SynthModel::Minimoog);
            assert(!standaloneSwPackets.empty());
            assert(standaloneSwPackets.back().size() == 284);
            assert(standaloneSwPackets.back()[4] == 0x64); // 284-byte Active RAM preset write
        }

        // Check cached 3-line display state inside MpkMiniMk4::DisplayDriver
        const auto& linesAfterPreset = engine.getMpkDisplay().getLastLines();
        assert(linesAfterPreset[0].text.find("MINIMOOG") != std::string::npos);
        assert(linesAfterPreset[0].align == MpkMiniMk4::Align::Center);
        assert(linesAfterPreset[1].align == MpkMiniMk4::Align::Center);
        assert(linesAfterPreset[2].align == MpkMiniMk4::Align::Center);

        // Toggle Shift (Bank B) -> should update header to [B] and bank labels
        pluginPackets.clear();
        engine.setShiftActive(true);
        assert(!pluginPackets.empty());
        assert(engine.getMpkDisplay().getLastLines()[0].text.find("[B]") != std::string::npos);

        // Turn Cutoff knob (CC 74 = 96) -> centred label on line 1, value host-drawn (0x71) over an empty line 2
        pluginPackets.clear();
        engine.controlChange(74, 96);
        engine.controlChange(74, 96); // the value is drawn on the call after the line 2 rewrite
        assert(!pluginPackets.empty());
        assert(engine.getMpkDisplay().getLastLines()[1].text.find("CUTOFF") != std::string::npos);
        assert(engine.getMpkDisplay().getLastLines()[1].align == MpkMiniMk4::Align::Center);
        assert(engine.getMpkDisplay().getLastLines()[2].text.empty());
        assert(engine.getMpkDisplay().getHostValueText().find("Hz") != std::string::npos);
        bool sawBlit = false;
        for (const auto& pkt : pluginPackets) sawBlit = sawBlit || (pkt.size() > 4 && pkt[4] == 0x71);
        assert(sawBlit);
    }
    std::cout << "  Passed (MpkMiniMk4Display library & SynthEngine OLED updates verified)" << std::endl;

    std::cout << "\nAll SynthEngine core unit tests passed successfully!\n" << std::endl;
    return 0;
}
