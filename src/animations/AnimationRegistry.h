#pragma once

#include "IAnimation.h"
#include <cstddef>

namespace HardwareDriver {

// Fixed-capacity registry for OLED animations.
// Zero dynamic allocation — uses a raw pointer array.
// Animations are registered at startup and cycled via next()/prev().
class AnimationRegistry {
public:
    static constexpr size_t MAX_ANIMATIONS = 16;

    AnimationRegistry() : count_(0), currentIndex_(0) {
        for (size_t i = 0; i < MAX_ANIMATIONS; ++i) {
            animations_[i] = nullptr;
        }
    }

    // Register an animation (must be a static/global object, not heap-allocated).
    // Returns true on success, false if registry is full.
    bool registerAnimation(IAnimation* anim) {
        if (count_ >= MAX_ANIMATIONS || anim == nullptr) return false;
        animations_[count_++] = anim;
        return true;
    }

    // Cycle to next animation
    void next() {
        if (count_ == 0) return;
        currentIndex_ = (currentIndex_ + 1) % count_;
        animations_[currentIndex_]->reset();
    }

    // Cycle to previous animation
    void prev() {
        if (count_ == 0) return;
        currentIndex_ = (currentIndex_ + count_ - 1) % count_;
        animations_[currentIndex_]->reset();
    }

    // Select animation by index
    void select(size_t index) {
        if (index < count_) {
            currentIndex_ = index;
            animations_[currentIndex_]->reset();
        }
    }

    // Get currently active animation (nullptr if empty)
    IAnimation* getActive() const {
        return (count_ > 0) ? animations_[currentIndex_] : nullptr;
    }

    // Forward event hooks to active animation
    void onNoteOn(uint8_t note, uint8_t velocity) {
        if (IAnimation* a = getActive()) a->onNoteOn(note, velocity);
    }
    void onNoteOff(uint8_t note) {
        if (IAnimation* a = getActive()) a->onNoteOff(note);
    }
    void onBeat(uint8_t beatIndex, bool isDownbeat) {
        if (IAnimation* a = getActive()) a->onBeat(beatIndex, isDownbeat);
    }

    size_t getIndex() const { return currentIndex_; }
    size_t count() const { return count_; }
    const char* getActiveName() const {
        IAnimation* a = getActive();
        return a ? a->getName() : "None";
    }

private:
    IAnimation* animations_[MAX_ANIMATIONS];
    size_t count_;
    size_t currentIndex_;
};

} // namespace HardwareDriver
