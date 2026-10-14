// Plinko: notes drop balls through pegs into eight bins (included by tests/test_toy_animations.cpp)

void test_plinko() {
    std::cout << "[Test] PlinkoAnimation..." << std::endl;
    PlinkoAnimation anim;
    anim.init(); prime(anim);
    assert(std::strcmp(anim.getName(), "Plinko") == 0);
    assert(anim.getPegCount() == 40);
    ToyVoices tv;
    run(anim, tv, 0.2f);
    assert(anim.getBallCount() == 0);

    // A three-note chord drops three balls at once; loud notes drop heavy balls
    tv.noteOn(48, 120); tv.noteOn(60, 120); tv.noteOn(72, 120);
    run(anim, tv, 0.05f);
    assert(anim.getBallCount() == 3);
    for (int i = 0; i < 3; ++i) assert(anim.getBallRadius(i) == 2);

    U8G2 d;
    d.begin();
    anim.render(d, tv.ctx);
    assert(blueZoneOnly(d));
    for (int x = 0; x < 128; ++x) assert(d.getPixel(x, 17) && d.getPixel(x, 62)); // rail and floor
    assert(d.getPixel(16, 55));                                                     // bin divider
    assert(d.getPixel(8, 28));                                                      // a peg

    // Edge keys land in the bin under the key: lowest key -> bin 0, highest -> bin 7
    tv.noteOff(48); tv.noteOff(60); tv.noteOff(72);
    run(anim, tv, 2.0f);
    assert(anim.getBallCount() == 0);
    assert(anim.getBinCount(0) == 1 && anim.getBinCount(7) == 1);
    int total = 0;
    for (int b = 0; b < PlinkoAnimation::BINS; ++b) total += anim.getBinCount(b);
    assert(total == 3);

    // Keep hitting the lowest key: bin 0 fills, rings DING at 5 balls, then empties
    bool dinged = false;
    for (int i = 0; i < 12 && !dinged; ++i) {
        tv.noteOn(48, 60);
        run(anim, tv, 0.1f);
        tv.noteOff(48);
        for (int k = 0; k < 30 && !dinged; ++k) { run(anim, tv, 0.05f); dinged = anim.isDinging(0); }
        if (!dinged) assert(anim.getBinCount(0) < PlinkoAnimation::BIN_FULL);
    }
    assert(dinged);
    assert(anim.getBinCount(0) == PlinkoAnimation::BIN_FULL);
    d.clearBuffer();
    anim.render(d, tv.ctx);
    assert(blueZoneOnly(d));
    assert(litPixels(d, 1, 43, 15, 47) > 5); // "DING" over bin 0
    run(anim, tv, 0.6f);
    assert(!anim.isDinging(0) && anim.getBinCount(0) == 0);
    assert(anim.getBinCount(7) == 1); // other bins untouched

    // The carriage rides to the last struck key
    tv.noteOn(72, 100);
    run(anim, tv, 0.5f);
    assert(anim.getCarriageX() > 115.0f);

    anim.reset(); prime(anim);
    assert(anim.getBallCount() == 0 && anim.getBinCount(7) == 0 && anim.getCarriageX() == 64.0f);
    std::cout << "  -> PlinkoAnimation tests PASSED!" << std::endl;
}
