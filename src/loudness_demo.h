#pragma once

// Loudness check (mk4-synth-loudness only): after boot, plays the PresetLoudnessTest phrase from
// CMakeLists.txt (chords then a single-note line, velocity 110) on every factory preset in turn, with
// 3 s of silence between presets. Record the DAC output and compare with synth_render's figures.

#ifdef SYNTH_LOUDNESS_DEMO

#include <Arduino.h>
#include "SynthEngine.h"
#include "Presets.h"

namespace LoudnessDemo {

struct Step { uint8_t notes[3]; uint8_t count; uint16_t ms; };

// "C3,E3,G3:1 r:0.3 C4,E4,G4:0.5 r:0.2 G3,B3,D4:0.5 r:0.2 F3,A3,C4:1 A2:0.25 C3:0.25 E3:0.25 A3:0.25
//  E4:0.25 C4:0.25 A3:0.5" (C4 = 60)
static constexpr Step kPhrase[] = {
    {{48, 52, 55}, 3, 1000}, {{0}, 0, 300}, {{60, 64, 67}, 3, 500}, {{0}, 0, 200},
    {{55, 59, 62}, 3, 500},  {{0}, 0, 200}, {{53, 57, 60}, 3, 1000},
    {{45}, 1, 250}, {{48}, 1, 250}, {{52}, 1, 250}, {{57}, 1, 250}, {{64}, 1, 250}, {{60}, 1, 250},
    {{57}, 1, 500},
};
static constexpr size_t kSteps = sizeof(kPhrase) / sizeof(kPhrase[0]);
static constexpr uint32_t kStartDelayMs = 8000, kGapMs = 3000;
static constexpr uint8_t kVelocity = 110;

class Player {
public:
    template <typename SelectPreset>
    void poll(Synth::SynthEngine& engine, SelectPreset selectPreset) {
        const uint32_t now = millis();
        if (preset_ >= Synth::FACTORY_PRESET_COUNT || now < nextMs_) return;
        if (nextMs_ == 0) { nextMs_ = now + kStartDelayMs; return; }

        if (step_ == 0 && !stepActive_) {
            selectPreset(preset_);
            Serial.printf("LOUDNESS preset %u start %lu ms\n", preset_ + 1, static_cast<unsigned long>(now));
        }
        if (stepActive_) {
            const Step& s = kPhrase[step_];
            for (uint8_t i = 0; i < s.count; ++i) engine.noteOff(s.notes[i]);
            stepActive_ = false;
            if (++step_ == kSteps) {
                step_ = 0;
                ++preset_;
                nextMs_ = now + kGapMs;
                if (preset_ == Synth::FACTORY_PRESET_COUNT) Serial.println("LOUDNESS done");
            }
            return poll(engine, selectPreset);
        }
        const Step& s = kPhrase[step_];
        for (uint8_t i = 0; i < s.count; ++i) engine.noteOn(s.notes[i], kVelocity);
        stepActive_ = true;
        nextMs_ = now + s.ms;
    }

private:
    uint8_t preset_{0};
    size_t step_{0};
    bool stepActive_{false};
    uint32_t nextMs_{0};
};

} // namespace LoudnessDemo

#endif
