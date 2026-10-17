// Choir: one blob singer per voice (included by tests/test_toy_animations.cpp)

void test_choir() {
    std::cout << "[Test] ChoirAnimation..." << std::endl;
    ChoirAnimation anim;
    anim.init(); prime(anim);
    assert(std::strcmp(anim.getName(), "Choir") == 0);
    ToyVoices tv;
    run(anim, tv, 0.5f);
    for (int v = 0; v < 8; ++v) assert(anim.getHop(v) == 0.0f && anim.getMouth(v) < 0.01f);

    // A chord: every struck voice's singer hops and opens its mouth; silent singers stay put
    tv.noteOn(48, 120); tv.noteOn(60, 120); tv.noteOn(84, 120);
    run(anim, tv, 0.1f);
    for (int v = 0; v < 3; ++v) assert(anim.getHop(v) < -2.0f);
    for (int v = 3; v < 8; ++v) assert(anim.getHop(v) == 0.0f);
    assert(anim.getNoteGlyphCount() == 3);
    run(anim, tv, 0.6f); // landed, sustaining at 0.6
    for (int v = 0; v < 3; ++v) {
        assert(anim.getHop(v) == 0.0f);
        assert(anim.getMouth(v) > 0.5f);
        assert(anim.getArms(v) > 0.5f);
    }
    assert(anim.getMouth(3) < 0.01f);
    // Higher notes stand taller: 48 -> 2.5 px, 60 -> 5, 84 -> 10
    assert(anim.getStretch(0) < anim.getStretch(1) && anim.getStretch(1) < anim.getStretch(2));
    assert(std::fabs(anim.getStretch(2) - 10.0f) < 0.5f);

    // Louder notes hop higher
    {
        ChoirAnimation soft, loud;
        soft.init(); loud.init(); prime(soft); prime(loud);
        ToyVoices a, b;
        a.noteOn(60, 10); b.noteOn(60, 127);
        float minSoft = 0.0f, minLoud = 0.0f;
        for (int i = 0; i < 8; ++i) {
            run(soft, a, 0.05f); run(loud, b, 0.05f);
            minSoft = std::fmin(minSoft, soft.getHop(0));
            minLoud = std::fmin(minLoud, loud.getHop(0));
        }
        assert(minLoud < minSoft - 1.0f);
    }

    // Fast arpeggio on voice 0 (e.g. 1-voice preset): rapid strikes must not launch singer off screen
    {
        ChoirAnimation arp;
        arp.init(); prime(arp);
        ToyVoices arpTv;
        U8G2 arpd;
        arpd.begin();
        constexpr float MAX_HOP = ChoirAnimation::MAX_HOP;

        for (int strike = 0; strike < 50; ++strike) { // 50 strikes every 50 ms = 2.5 s
            arpTv.noteOn(60, 127);
            run(arp, arpTv, 0.025f, 0.025f);
            arpTv.noteOff(60);
            run(arp, arpTv, 0.025f, 0.025f);
            assert(arp.getHop(0) >= -MAX_HOP);
            assert(arp.getHop(0) <= 0.0f);
            arpd.clearBuffer();
            arp.render(arpd, arpTv.ctx);
            assert(blueZoneOnly(arpd));
        }
        // Voice 0 singer is still visible on screen in the blue zone
        assert(litPixels(arpd, 0, 16, 127, 61) > 0);
    }

    // Mono voice mode (PT-10 presets): every note lands on voice slot 0, but each key hops its
    // own singer (note % 8) rather than always the leftmost one
    {
        ChoirAnimation mono;
        mono.init(); prime(mono);
        ToyVoices monoTv;
        monoTv.ctx.monoVoice = true;
        const uint8_t keys[3] = {60, 62, 65}; // singers 4, 6, 1
        for (uint8_t key : keys) {
            monoTv.noteOn(key, 120);
            run(mono, monoTv, 0.1f);
            const int singer = key % 8;
            assert(mono.getHop(singer) < -2.0f);
            for (int v = 0; v < 8; ++v) if (v != singer) assert(mono.getHop(v) > -2.0f);
            monoTv.noteOff(key);
            run(mono, monoTv, 0.8f); // released and landed
            assert(mono.getMouth(singer) < 0.05f);
            for (int v = 0; v < 8; ++v) assert(mono.getHop(v) == 0.0f);
        }
        // Repeating a key re-hops the same singer
        monoTv.noteOn(60, 120);
        run(mono, monoTv, 0.1f);
        assert(mono.getHop(4) < -2.0f);
        assert(mono.getHop(0) == 0.0f);
        // Legato: slot 0 changes note without retriggering; the new note's singer still hops
        run(mono, monoTv, 0.8f);
        monoTv.v[0].note = 67;
        run(mono, monoTv, 0.1f);
        assert(mono.getHop(3) < -2.0f && mono.getHop(4) == 0.0f);
    }

    U8G2 d;
    d.begin();
    anim.render(d, tv.ctx);
    assert(blueZoneOnly(d));
    for (int x = 0; x < 128; ++x) assert(d.getPixel(x, 62)); // floor line
    assert(d.getPixel(ChoirAnimation::slotX(0) - 6, 50));        // singer 0's left side
    assert(litPixels(d, 0, 16, 127, 61) > 100);

    // Release everything; after 6 s of quiet the choir sags and sleeps with z's floating up
    tv.noteOff(48); tv.noteOff(60); tv.noteOff(84);
    run(anim, tv, 5.0f);
    assert(!anim.isAsleep());
    run(anim, tv, 2.5f);
    assert(anim.isAsleep());
    assert(anim.getZCount() > 0);
    assert(anim.getStretch(4) < -2.0f);
    d.clearBuffer();
    anim.render(d, tv.ctx);
    assert(blueZoneOnly(d));

    // Playing wakes them and clears the z's; reset() restores the initial state
    tv.noteOn(60, 100);
    run(anim, tv, 0.1f);
    assert(!anim.isAsleep() && anim.getZCount() == 0);
    anim.reset(); prime(anim);
    assert(anim.getNoteGlyphCount() == 0 && anim.getHop(0) == 0.0f && !anim.isAsleep());
    std::cout << "  -> ChoirAnimation tests PASSED!" << std::endl;
}
