// WashAnimation tests (included by tests/test_toy_animations.cpp)

// Largest distance of any garment from its own machine's drum centre
static float washMaxClothRadius(const WashAnimation& anim) {
    float maxR = 0.0f;
    for (int m = 0; m < WashAnimation::M; ++m) {
        for (int i = 0; i < anim.getClothCount(m); ++i) {
            const float dx = anim.getClothX(m, i) - WashAnimation::drumX(m);
            const float dy = anim.getClothY(m, i) - WashAnimation::CY;
            const float r = std::sqrt(dx * dx + dy * dy);
            if (r > maxR) maxR = r;
        }
    }
    return maxR;
}

void test_wash() {
    std::cout << "[Test] WashAnimation..." << std::endl;
    WashAnimation anim;
    anim.init(); prime(anim);
    assert(std::strcmp(anim.getName(), "Laundromat") == 0);
    ToyVoices tv;
    run(anim, tv, 0.5f);
    for (int m = 0; m < WashAnimation::M; ++m) {
        assert(anim.getClothCount(m) == 0);
        assert(anim.getWater(m) < 0.01f);
    }

    // The keybed is split in thirds: low, mid and high notes load machines 0, 1 and 2
    const uint8_t thirds[3] = {48, 60, 72};
    for (int m = 0; m < 3; ++m) {
        tv.noteOn(thirds[m], 80);
        run(anim, tv, 0.05f);
        for (int k = 0; k < 3; ++k) assert(anim.getClothCount(k) == (k <= m ? 1 : 0));
        assert(anim.getClothKind(m, 0) == thirds[m] % 3);
        assert(anim.getShake(m) > 0.3f); // hit rocks this machine only
        for (int k = m + 1; k < 3; ++k) assert(anim.getShake(k) == 0.0f);
        tv.noteOff(thirds[m]);
        run(anim, tv, 0.3f);
    }

    // Each note throws in one garment; capped at 5 per machine (the oldest comes out)
    for (int k = 0; k < 7; ++k) {
        const uint8_t note = static_cast<uint8_t>(65 + k);
        tv.noteOn(note, 80);
        run(anim, tv, 0.05f);
        assert(anim.getClothCount(2) == (k + 2 < 5 ? k + 2 : 5));
        assert(anim.getClothKind(2, anim.getClothCount(2) - 1) == note % 3);
        tv.noteOff(note);
        run(anim, tv, 0.3f);
    }
    assert(anim.getClothCount(0) == 1 && anim.getClothCount(1) == 1 && anim.getClothCount(2) == 5);
    run(anim, tv, 1.5f);
    for (int m = 0; m < WashAnimation::M; ++m) assert(anim.getWater(m) < 0.05f);

    // Drums turn a quarter turn per beat: pi rad/s at 120 BPM
    const float calmSpeed = anim.getDrumSpeed(0);
    assert(std::fabs(calmSpeed - 3.14159265f) < 0.05f);

    // Two notes held in the low third: water rises there only; after 1 s only machine 0 spins
    tv.noteOn(50, 100); tv.noteOn(53, 100);
    run(anim, tv, 0.8f);
    assert(anim.getSpin(0) < 0.05f);
    assert(anim.getWater(0) > 0.3f);
    assert(anim.getWater(1) < 0.05f && anim.getWater(2) < 0.05f);
    float maxR = 0.0f;
    for (int f = 0; f < 60; ++f) {
        run(anim, tv, 0.05f);
        const float r = washMaxClothRadius(anim);
        if (r > maxR) maxR = r;
    }
    assert(anim.getSpin(0) > 0.9f);
    assert(anim.getSpin(1) == 0.0f && anim.getSpin(2) == 0.0f);
    assert(anim.getDrumSpeed(0) > calmSpeed * 5.0f);
    assert(std::fabs(anim.getDrumSpeed(1) - calmSpeed) < 0.01f);
    assert(anim.getWater(1) < 0.05f && anim.getWater(2) < 0.05f);
    assert(maxR <= WashAnimation::R - 2.5f + 0.01f); // clothes stay inside their own drum

    U8G2 d;
    d.begin();
    anim.render(d, tv.ctx);
    assert(blueZoneOnly(d));
    assert(litPixels(d, 0, 17, 127, 62) > 300);

    // Release: spin and water settle
    tv.noteOff(50); tv.noteOff(53);
    run(anim, tv, 5.0f);
    assert(anim.getSpin(0) < 0.05f);
    assert(anim.getWater(0) < 0.05f);

    // Clothes are taken out after 12 s
    run(anim, tv, 12.0f);
    for (int m = 0; m < WashAnimation::M; ++m) assert(anim.getClothCount(m) == 0);

    // The lit panel light chases across all six lights (two per machine) on the beat
    tv.ctx.metronomeOn = true;
    for (int b = 0; b < 12; ++b) {
        tv.ctx.currentBeat = b;
        run(anim, tv, 0.05f);
        assert(anim.getBeat() == b);
        assert(anim.getLitLight() == b % 6);
    }
    tv.ctx.metronomeOn = false;

    // Render at rest: three cabinets at their home positions, each with its door ring
    U8G2 d2;
    d2.begin();
    anim.render(d2, tv.ctx);
    assert(blueZoneOnly(d2));
    for (int m = 0; m < WashAnimation::M; ++m) {
        const int x0 = 1 + m * WashAnimation::W;
        assert(d2.getPixel(x0, 40) && d2.getPixel(x0 + WashAnimation::W - 2, 40)); // cabinet sides
        assert(d2.getPixel(x0 + 20, 23));                                            // panel line
        assert(d2.getPixel(WashAnimation::drumX(m), WashAnimation::CY - WashAnimation::R - 3)); // door
    }

    anim.reset(); prime(anim);
    assert(anim.getClothCount(0) == 0 && anim.getSpin(0) == 0.0f);
    std::cout << "  -> WashAnimation tests PASSED!" << std::endl;
}
