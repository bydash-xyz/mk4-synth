#pragma once

#include "Voice.h"
#include "Parameters.h"
#include "Chorus.h"
#include "Presets.h"
#include "SysExProtocol.h"
#include "MpkMiniMk4Display.h"
#include <array>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace Synth {

using EngineRecursiveMutex = std::recursive_mutex;

struct HeldKey {
    uint8_t noteNumber; // pitch after scale quantization (used for note priority)
    uint8_t velocity;
    uint8_t rawNote;    // physical key, so releases still match after a key/scale change
};

// Lets a platform render part of the voices on another CPU core. start() must make the
// worker call SynthEngine::renderWorkerShare(); wait() returns once it has finished (false = timed out).
class VoiceWorker {
public:
    virtual ~VoiceWorker() = default;
    virtual void start() = 0;
    virtual bool wait() = 0;
};

class SynthEngine {
public:
    static constexpr size_t MAX_VOICES = 8;
    static constexpr float DEFAULT_ACCENT_AMOUNT = 0.7f; // presets without an Accent knob (CC 88)
    static constexpr float DEFAULT_LFO_RATE_HZ   = 5.0f; // presets without a vibrato rate knob (CC 89)
    using DisplayUpdateCallback = std::function<void(const Preset& newPreset, bool isShift)>;

    SynthEngine();

    void init(float sampleRate = SynthConfig::DEFAULT_SAMPLE_RATE);
    void setSampleRate(float sampleRate);

    // Preset & Model switching
    void loadPreset(SynthModel model);
    void loadPresetIndex(size_t index);
    void nextPreset();
    void prevPreset();
    const Preset& getCurrentPreset() const;

    // Shift / Bank B switching
    bool isShiftActive() const { return shiftActive_; }
    void setShiftActive(bool shift);
    void toggleShift();
    const KnobBank& getActiveKnobBank() const;

    void setOnDisplayUpdate(DisplayUpdateCallback cb) { onDisplayUpdate_ = cb; }

    // Akai MPK Mini MK4 (IV) Standalone OLED Display Library Integration
    MpkMiniMk4::DisplayDriver& getMpkDisplay() { return mpkDisplay_; }
    const MpkMiniMk4::DisplayDriver& getMpkDisplay() const { return mpkDisplay_; }
    void setMpkDisplaySendCallbacks(MpkMiniMk4::DisplayDriver::SendPacketFn pluginPortSend,
                                    MpkMiniMk4::DisplayDriver::SendPacketFn swControlPortSend = nullptr);
    void updateMpkMiniDisplay(const char* line1Override = nullptr, const char* line2Override = nullptr);
    void notifyMpkMiniParameterChanged(uint8_t ccNumber, uint8_t value);
    // Human-readable name + value (e.g. "K3: CUTOFF" / "1200 Hz >") for a CC; false if the CC has no display name
    bool describeParameter(uint8_t ccNumber, uint8_t value, std::string& outLabel, std::string& outValue);
    // The CC value whose controlChange() reproduces the live sound for `cc` (the inverse of its scaling), with
    // SHIFT in state `shift`. Discrete knobs keep `current` when it already selects the live state.
    uint8_t knobValueFor(uint8_t cc, bool shift, uint8_t current) const;

    // Voice Mode & Note Priority (Monophonic / Polyphonic)
    void setVoiceMode(VoiceMode mode);
    VoiceMode getVoiceMode() const { return voiceMode_; }

    void setNotePriority(NotePriority priority) { notePriority_ = priority; }

    void setPortamentoEnabled(bool enabled);
    bool isPortamentoEnabled() const { return portamentoEnabled_; }

    void setGlideTimeMs(float ms);
    float getGlideTimeMs() const { return glideTimeMs_; }

    void setLegatoEnabled(bool enabled) { legatoEnabled_ = enabled; }
    bool isLegatoEnabled() const { return legatoEnabled_; }

    size_t getHeldKeyCount() const { return heldKeys_.size(); }

    // MIDI Event handlers
    void noteOn(uint8_t noteNumber, uint8_t velocity);
    void noteOff(uint8_t noteNumber);
    void controlChange(uint8_t ccNumber, uint8_t value);
    void pitchBend(int16_t bendValue); // 0 to 16383, center 8192
    void allNotesOff();
    void panic(); // Immediately cut all voices

    // Audio block rendering
    // Writes interleaved 16-bit stereo PCM [L, R, L, R, ...]
    void processBlock(int16_t* outStereoInterleaved, size_t numFrames);

    // Unified core float audio generation pipeline (vibrato LFO, voice mix, chorus)
    void renderInternal(float* outL, float* outR, size_t numFrames);

    // Audio block rendering in floating-point for testing or internal routing
    void processBlockFloat(float* outLeft, float* outRight, size_t numFrames);

    // Polyphony voice limit (up to MAX_VOICES)
    void setVoiceLimit(size_t limit);
    size_t getVoiceLimit() const { return voiceLimit_; }
    // Voices actually used: the platform limit capped by the modelled instrument's polyphony
    size_t getEffectiveVoiceLimit() const;

    // Thread-safe event mode for targets where MIDI/UI and audio run on different threads/cores.
    // When enabled, note events and voice parameter pushes are queued and applied by the audio
    // thread at the start of each render call, so only the audio thread ever touches Voice state.
    // Disabled (immediate) by default so single-threaded callers and unit tests see effects at once.
    void setDeferredEvents(bool deferred);
    bool isDeferredEvents() const { return deferredEvents_; }

    // Optional second render core: with 3+ sounding voices, half of them are rendered by the worker
    // while the audio thread renders the rest. Unset (desktop, tests) = everything on the caller.
    void setVoiceWorker(VoiceWorker* worker) { voiceWorker_ = worker; }
    void renderWorkerShare(); // called by the VoiceWorker's thread between start() and wait()

    // Per-model character in use (factory model whose voicing applies, Count = generic user)
    SynthModel getVoicingModel() const { return voicingModel_; }
    const ModelVoicing& getVoicing() const { return voicing_; }

    void setAccentAmount(float amount);
    float getAccentAmount() const { return accentAmount_; }

    // Read-only voice access for diagnostics and tests
    const Voice& getVoice(size_t index) const { return voices_[index % MAX_VOICES]; }

    // SysEx handling & callbacks
    using SysExSendCallback = std::function<void(const uint8_t* data, size_t len)>;
    void handleSysEx(const uint8_t* data, size_t len);
    // Same, also returning this message's reply (handshake or dump; empty if none) in `reply`. The reply
    // is written under the engine lock, so concurrent callers on other tasks cannot mix replies up.
    void handleSysEx(const uint8_t* data, size_t len, std::vector<uint8_t>& reply);
    void setOnSysExSend(SysExSendCallback cb) { onSysExSend_ = cb; }
    const std::vector<uint8_t>& getLastSysExResponse() const { return lastSysExResponse_; }

    // User Preset Slots (USER_SLOT_START..USER_SLOT_END, Slots #24..#32) and Non-Volatile Storage persistence
    // Blobs are PatchDumpV2 bodies (variable length, at most MAX_PATCH_BODY_SIZE). The read callback
    // returns the number of bytes read, 0 if the slot has none. The optional v1 read callback returns a
    // legacy 48-byte RawPatchData blob, used when the slot has no v2 blob yet.
    using StorageWriteCallback   = std::function<bool(uint8_t slot, const uint8_t* data, size_t size)>;
    using StorageReadCallback    = std::function<size_t(uint8_t slot, uint8_t* outData, size_t maxSize)>;
    using V1StorageReadCallback  = std::function<bool(uint8_t slot, uint8_t* outData, size_t size)>;

    void setStorageCallbacks(StorageWriteCallback writeCb, StorageReadCallback readCb,
                             V1StorageReadCallback v1ReadCb = nullptr);
    bool saveUserPresetToStorage(uint8_t slot = USER_PRESET_SLOT);
    bool loadUserPresetFromStorage(uint8_t slot = USER_PRESET_SLOT);
    bool loadAllUserPresetsFromStorage();
    Preset& getUserPreset(uint8_t slot = USER_PRESET_SLOT);
    const Preset& getUserPreset(uint8_t slot = USER_PRESET_SLOT) const;

    // Parameter getters & setters
    void setCutoffHz(float cutoffHz);
    void setResonanceQ(float q);
    void setWaveform(Waveform wave);
    void setSubOscLevel(float lvl);
    void setDetuneCents(float cents);
    void setDrive(float drive);
    void setChorusEnabled(bool enabled);
    void setChorusMix(float mix);

    void setAmpAttackMs(float ms);
    void setAmpDecayMs(float ms);
    void setAmpSustainLevel(float level);
    void setAmpReleaseMs(float ms);

    void setFilterAttackMs(float ms);
    void setFilterDecayMs(float ms);
    void setFilterSustainLevel(float level);
    void setFilterReleaseMs(float ms);

    void setFilterEnvAmount(float amount);
    void setMasterVolume(float vol);

    float getCutoffHz() const { return cutoffHz_; }
    float getResonanceQ() const { return resonanceQ_; }
    Waveform getWaveform() const { return waveform_; }
    float getSubOscLevel() const { return subOscLevel_; }
    float getDetuneCents() const { return detuneCents_; }
    float getDrive() const { return drive_; }
    float getFilterEnvAmount() const { return filterEnvAmount_; }
    bool  isChorusEnabled() const;
    float getChorusMix() const;
    float getMasterVolume() const { return masterGain_; }

    float getAmpAttackMs() const { return ampAttackMs_; }
    float getAmpDecayMs() const { return ampDecayMs_; }
    float getAmpSustainLevel() const { return ampSustainLevel_; }
    float getAmpReleaseMs() const { return ampReleaseMs_; }

    float getFilterAttackMs() const { return filterAttackMs_; }
    float getFilterDecayMs() const { return filterDecayMs_; }
    float getFilterSustainLevel() const { return filterSustainLevel_; }
    float getFilterReleaseMs() const { return filterReleaseMs_; }

    void setChorusRateHz(float rateHz);
    float getChorusRateHz() const;
    void setChorusDepthMs(float depthMs);
    float getChorusDepthMs() const;

    void setPulseWidth(float pw);
    float getPulseWidth() const { return pulseWidth_; }

    void setVibratoDepth(float depth);
    float getVibratoDepth() const { return vibratoDepth_; }

    void setVibratoRateHz(float rateHz);
    float getVibratoRateHz() const { return lfoRateHz_; }

    void setCasioTone(CasioTone tone);
    CasioTone getCasioTone() const { return casioTone_; }
    const char* getCasioToneName() const;

    // FM Synthesis & Tremolo getters & setters
    void setModRatio(float ratio);
    float getModRatio() const { return modRatio_; }

    void setModDepth(float depth);
    float getModDepth() const { return modDepth_; }

    void setModFeedback(float fb);
    float getModFeedback() const { return modFeedback_; }

    void setTremoloDepth(float depth);
    float getTremoloDepth() const { return tremoloDepth_; }

    void setTremoloRateHz(float rateHz);
    float getTremoloRateHz() const { return tremoloRateHz_; }

    void setSustainPedal(bool held);
    bool isSustainPedalHeld() const { return sustainPedalHeld_; }

    EngineMode getEngineMode() const { return engineMode_; }
    static const char* getEngineModeName(EngineMode mode);

    // Wavetable / phase distortion / pluck / additive parameters (clamped to their ranges)
    void setTimbre(const TimbreParams& timbre);
    const TimbreParams& getTimbre() const { return timbre_; }
    void setWavetablePosition(float pos);
    void setWavetableEnvAmount(float amount);
    void setWavetable(uint8_t table);
    void setPdDcw(float dcw);
    void setPdWave(PdWave wave);
    void setPluckDecayMs(float ms);
    void setPluckBrightness(float bright);
    void setPluckPickPosition(float pick);
    void setDrawbar(size_t index, float level); // 0..8
    void setPercussionLevel(float level);
    void setPercussionHarmonic(uint8_t harmonic); // 0 = second, 1 = third
    void setKeyClick(float level);

    size_t getActiveVoiceCount() const;

    // =========================================================================
    // Animation Context API (for OLED display)
    // =========================================================================
    uint8_t getLastNoteNumber() const { return lastNoteNumber_; }
    uint8_t getLastVelocity() const { return lastVelocity_; }
    uint32_t getLastNoteOnMillis() const { return lastNoteOnMillis_; }
    // 0..1 position within the current metronome beat (0 when the metronome is off)
    float getBeatPhase() const;

    // Snapshot of per-voice state for display (avoids exposing Voice internals)
    void getVoiceSnapshot(uint8_t outNotes[8], float outEnvelopes[8], uint8_t& outCount) const;

    // =========================================================================
    // Metronome, MIDI Clock (24 PPQN), Time Signature & Musical Key/Scale API
    // =========================================================================
    void setMetronomeEnabled(bool enabled);
    bool isMetronomeEnabled() const { return metronomeEnabled_; }
    void toggleMetronome();

    void setMetronomeVolume(float vol);
    float getMetronomeVolume() const { return metronomeVolume_; }

    void setMetronomeBpm(float bpm);
    float getMetronomeBpm() const { return metronomeBpm_; }
    void tapTempo(uint32_t nowMs);

    void setMetronomeKeyTuned(bool keyTuned) { metronomeKeyTuned_ = keyTuned; }
    bool isMetronomeKeyTuned() const { return metronomeKeyTuned_; }

    // Silent metronome: beats keep counting (beat LEDs, OLED) but no click reaches the audio output,
    // so it can run while recording.
    void setMetronomeSilent(bool silent);
    bool isMetronomeSilent() const { return metronomeSilent_; }

    // Mono output: the L+R mix on the left channel and silence on the right. Lets a single TRS cable
    // into one balanced input (which hears L-R) still carry presets without stereo effects.
    void setMonoOutput(bool mono) { monoOutput_ = mono; }
    bool isMonoOutput() const { return monoOutput_; }

    // Real-time MIDI Clock (0xF8 = 24 PPQN, 0xFA = Start, 0xFC = Stop)
    void midiClockTick();
    void midiClockStart();
    void midiClockStop();
    bool isExternalClockActive() const { return externalClockActive_; }

    // Time Signature (e.g. 4/4, 3/4, 2/4, 5/4, 6/8, 7/8, 9/8, 12/8)
    void setTimeSignature(uint8_t beatsPerBar, uint8_t beatUnit);
    void setTimeSignaturePreset(TimeSignaturePreset preset);
    uint8_t getTimeSigBeatsPerBar() const { return timeSigNumerator_; }
    uint8_t getTimeSigBeatUnit() const { return timeSigDenominator_; }
    uint8_t getCurrentBeat() const { return currentBeatIndex_; }
    // Bumped on every metronome beat, audible or silent (audio thread); the UI polls it to flash beat LEDs.
    uint32_t getMetroClickCount() const { return metroClickCount_; }
    bool isLastMetroClickDownbeat() const { return lastMetroClickDownbeat_; }
    const char* getTimeSignatureString() const;

    // Musical Key (0=C .. 11=B) & Scale Mode Quantizer
    void setRootKey(uint8_t rootKeySemitone);
    uint8_t getRootKey() const { return rootKey_; }
    const char* getRootKeyName() const;

    void setScaleMode(ScaleMode mode);
    const char* getScaleModeName() const;

    uint8_t quantizeNoteToKeyAndScale(uint8_t midiNote) const;

private:
    float sampleRate_{SynthConfig::DEFAULT_SAMPLE_RATE};
    std::array<Voice, MAX_VOICES> voices_;
    uint32_t voiceCounter_{0};
    size_t voiceLimit_{MAX_VOICES};

    // Active Preset & Bank
    size_t currentPresetIndex_{0};
    bool   shiftActive_{false};
    DisplayUpdateCallback onDisplayUpdate_;
    MpkMiniMk4::DisplayDriver mpkDisplay_{0x00};

    // Effects
    StereoChorus chorus_;
    SolinaEnsemble solinaEnsemble_;
    float hypePumpPhase_{0.0f};
    // Static render scratch (2 x 8 KB): heap blocks > 4 KB would land in PSRAM on ESP32-S3
    std::array<float, 2048> tempL_{};
    std::array<float, 2048> tempR_{};

    // Voice rendering works on chunks: each voice fills its own pass over the chunk, so a share of
    // the voices can run on the worker core. Buffers are members to keep the audio stack small.
    static constexpr size_t RENDER_CHUNK = 128;
    static constexpr size_t MIN_VOICES_FOR_WORKER = 3;
    VoiceWorker* voiceWorker_{nullptr};
    float    vibChunk_[RENDER_CHUNK];
    float    mixMain_[RENDER_CHUNK];
    float    mixWorker_[RENDER_CHUNK];
    uint8_t  workerVoices_[MAX_VOICES];
    size_t   workerVoiceCount_{0};
    size_t   workerFrames_{0};
    void renderVoices(const uint8_t* indices, size_t count, size_t frames, float* acc);

    // Instrument voicing (filter topology, key tracking, velocity, accent, drift, polyphony)
    SynthModel   voicingModel_{SynthModel::Juno60};
    // The patch's own voicing (factory model, or Count = generic); voicingModel_ is it resolved for the engine mode
    SynthModel   patchVoicing_{SynthModel::Juno60};
    ModelVoicing voicing_{getModelVoicing(SynthModel::Juno60)};
    float        accentAmount_{DEFAULT_ACCENT_AMOUNT};

    // Deferred (audio-thread-applied) events; single producer (callers serialize via engineMutex_)
    enum class EventType : uint8_t { NoteOn, NoteOff, AllNotesOff, Panic, Sustain, FastReleaseAll };
    struct PendingEvent { EventType type; uint8_t a; uint8_t b; };
    static constexpr uint32_t EVENT_QUEUE_SIZE = 256; // power of 2
    std::array<PendingEvent, EVENT_QUEUE_SIZE> eventQueue_{};
    std::atomic<uint32_t> eventWrite_{0};
    std::atomic<uint32_t> eventRead_{0};
    std::atomic<bool>     voicesDirty_{false};
    bool                  deferredEvents_{false};

    void pushEvent(EventType type, uint8_t a = 0, uint8_t b = 0);
    void drainEvents();
    void dispatchEvent(const PendingEvent& e);
    void updateAllVoices(); // apply parameters now, or mark dirty in deferred mode
    template <typename F> void updateVoices(F&& fn) {
        if (deferredEvents_) {
            voicesDirty_.store(true, std::memory_order_release);
            return;
        }
        for (auto& voice : voices_) fn(voice);
    }

    // Immediate implementations (audio thread in deferred mode)
    void noteOnImpl(uint8_t noteNumber, uint8_t velocity);
    void noteOffImpl(uint8_t noteNumber);
    void allNotesOffImpl();
    void panicImpl();
    void setSustainPedalImpl(bool held);
    void fastReleaseAllImpl();

    // Global parameters
    Waveform waveform_{Waveform::Sawtooth};
    float subOscLevel_{0.0f};
    float detuneCents_{8.0f};
    float drive_{1.0f};
    float cutoffHz_{2000.0f};
    float resonanceQ_{1.0f};
    float filterEnvAmount_{0.5f};
    float masterGain_{0.7f};
    float levelGain_{1.0f};   // Per-model loudness trim (getModelLevelDb), so presets play at one level
    float outputGain_{0.7f};  // masterGain_ * levelGain_, the target of currentGain_
    float currentGain_{0.7f}; // Smoothed gain to eliminate zipper clicks and static
    float pitchBendSemitones_{0.0f};
    float modWheel_{0.0f};
    float pulseWidth_{0.5f};
    float vibratoDepth_{0.0f};
    CasioTone casioTone_{CasioTone::Fantasy};

    // Amp Envelope defaults
    float ampAttackMs_{10.0f};
    float ampDecayMs_{200.0f};
    float ampSustainLevel_{0.7f};
    float ampReleaseMs_{300.0f};

    // Filter Envelope defaults
    float filterAttackMs_{20.0f};
    float filterDecayMs_{400.0f};
    float filterSustainLevel_{0.3f};
    float filterReleaseMs_{300.0f};

    // LFO for vibrato / mod wheel
    float lfoPhase_{0.0f};
    float lfoRateHz_{DEFAULT_LFO_RATE_HZ};

    // Wavetable / phase distortion / pluck / additive parameters
    TimbreParams timbre_{};
    template <typename F> void editTimbre(F&& edit) {
        edit(timbre_);
        setTimbre(timbre_);
    }

    // FM Synthesis & Tremolo parameters
    EngineMode engineMode_{EngineMode::Subtractive};
    float modRatio_{1.0f};
    float modDepth_{2.0f};
    float modFeedback_{0.0f};
    float tremoloDepth_{0.0f};
    float tremoloRateHz_{4.5f};
    float tremoloPhase_{0.0f};
    bool  sustainPedalHeld_{false};

    // Animation context tracking
    uint8_t lastNoteNumber_{0};
    uint8_t lastVelocity_{0};
    uint32_t lastNoteOnMillis_{0};

    // Monophonic & Portamento state
    VoiceMode    voiceMode_{VoiceMode::Polyphonic};
    NotePriority notePriority_{NotePriority::Last};
    bool         portamentoEnabled_{false};
    float        glideTimeMs_{0.0f};
    bool         legatoEnabled_{true};
    std::vector<HeldKey> heldKeys_;

    Voice* allocateVoice(uint8_t noteNumber);
    void applyParametersToVoice(Voice& voice);
    void applyParametersToAllVoices();

    const HeldKey* resolveMonophonicNote() const;
    void noteOnMono(uint8_t noteNumber, uint8_t velocity, uint8_t rawNote);
    void noteOffMono(uint8_t rawNote);
    void removeHeldKey(uint8_t rawNote);

    // User Preset Slots (USER_SLOT_START..USER_SLOT_END, Slots #24..#32) and Storage Persistence
    static constexpr size_t NUM_USER_PRESETS = USER_PRESET_COUNT; // 9
    size_t slotToUserIndex(uint8_t slot) const {
        if (slot >= USER_SLOT_START && slot <= USER_SLOT_END) {
            return static_cast<size_t>(slot - USER_SLOT_START);
        }
        return 0;
    }

    std::array<std::array<char, 16>, NUM_USER_PRESETS> userPresetNames_{};
    std::array<Preset, NUM_USER_PRESETS> userPresets_{};
    // Last saved body per slot: keeps saves within a session when no storage callback persists them
    std::array<std::vector<uint8_t>, NUM_USER_PRESETS> userStorageMemory_{};
    StorageWriteCallback  storageWriteCb_;
    StorageReadCallback   storageReadCb_;
    V1StorageReadCallback storageV1ReadCb_;

    // SysEx Dispatch & Reply
    SysExSendCallback    onSysExSend_;
    std::vector<uint8_t> lastSysExResponse_;

    void sendSysExResponse(const uint8_t* data, size_t len);
    void sendHandshake(SysExCommand refCmd, HandshakeStatus status);
    void applyLiveParam(ParamId id, float value);
    void setEngineModeAndVoicing(EngineMode mode, SynthModel patchVoicing);
    void replyWithPatchDump(uint8_t slot, const Preset& preset);
    std::vector<uint8_t>* replyOut_{nullptr};
    void resetUserPreset(size_t uIdx);
    void applyPreset(const Preset& p);
    void syncUserKnobDefaults();
    void snapshotActiveParameters(Preset& target) const;

    mutable EngineRecursiveMutex engineMutex_;

    // Metronome, Clock, Time Signature & Musical Key/Scale State
    bool     metronomeEnabled_{false};
    float    metronomeVolume_{0.65f};
    float    metronomeBpm_{120.0f};
    bool     metronomeKeyTuned_{true}; // Tune beat 1 accent to active Root Key (C6..B6)
    bool     metronomeSilent_{false};
    bool     monoOutput_{false};
    uint8_t  timeSigNumerator_{4};
    uint8_t  timeSigDenominator_{4};
    uint8_t  currentBeatIndex_{0};
    uint32_t metroSampleCounter_{0};
    uint32_t metroClickCount_{0};
    bool     lastMetroClickDownbeat_{false};

    // DSP Click voice state (zero polyphonic voice stealing)
    float    metroPhase_{0.0f};
    float    metroBaseFreq_{1046.5f};
    float    metroClickEnv_{0.0f};
    float    metroClickDecay_{0.9985f};
    float    metroPitchEnv_{0.0f};
    float    metroPitchDecay_{0.990f};

    // External MIDI Clock (0xF8 = 24 PPQN) & Tap Tempo state
    bool     externalClockActive_{false};
    uint8_t  midiClockTickCount_{0};
    uint32_t samplesSinceLastClock_{0};
    uint32_t lastTapTimeMs_{0};

    // Musical Key (0=C .. 11=B) & Scale Quantizer state
    uint8_t   rootKey_{0}; // 0 = C
    ScaleMode scaleMode_{ScaleMode::Chromatic};

    void triggerMetroClick(uint8_t beatIdx);
    uint32_t metroSamplesPerBeat() const; // Internal-clock beat length at the current BPM & time signature
    bool isSecondaryCompoundAccent(uint8_t beatIdx) const;
};

} // namespace Synth
