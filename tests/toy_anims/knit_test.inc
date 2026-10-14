// KnitAnimation tests (included by tests/test_toy_animations.cpp)

void test_knit() {
    std::cout << "[Test] KnitAnimation..." << std::endl;
    KnitAnimation kn;
    kn.init(); prime(kn);
    assert(std::strcmp(kn.getName(), "Knit") == 0);
    assert(kn.isParked());

    ToyVoices tv;   // 120 BPM, metronome off: the carriage free-runs at 4 rows/s
    {
        // Parked and silent: nothing is knitted
        run(kn, tv, 1.0f);
        assert(kn.isParked() && kn.getRowCount() == 0);
        U8G2 d;
        d.begin();
        kn.render(d, tv.ctx);
        assert(blueZoneOnly(d));
        assert(d.getPixel(2, 17));   // "PARK" label
    }

    // A held note: strike leaves a knot (2), then V stitches (1) in its column
    const int col = KnitAnimation::colFor(60, tv.ctx);
    assert(col == 16);
    assert(KnitAnimation::colFor(48, tv.ctx) == 1 && KnitAnimation::colFor(72, tv.ctx) == 31);
    tv.noteOn(60, 100);
    run(kn, tv, 0.5f);
    assert(!kn.isParked());
    const uint32_t afterOne = kn.getCommits();
    run(kn, tv, 2.0f);
    // Two rows per beat at 120 BPM = 8 rows in 2 s
    const uint32_t rows2s = kn.getCommits() - afterOne;
    assert(rows2s >= 7 && rows2s <= 9);
    assert(kn.getRowCount() > 6);

    int knots = 0, stitches = 0, stray = 0;
    for (int i = 0; i < kn.getRowCount(); ++i) {
        for (int c = 0; c < KnitAnimation::COLS; ++c) {
            const uint8_t v = kn.getRow(i, c);
            if (v == 2) { ++knots; assert(c == col); }
            if (v == 1) { if (c == col) ++stitches; else if (c < col - 1 || c > col + 1) ++stray; }
        }
    }
    assert(knots == 1);          // exactly one knot for one strike (the oldest row)
    assert(kn.getRow(kn.getRowCount() - 1, col) == 2);
    assert(stitches >= 6);       // the sustain keeps knitting its column
    assert(stray == 0);          // loud attack widens to +-1 column at most (floor(env * 1.6))
    assert(kn.getRow(0, col - 1) == 0 && kn.getRow(0, col + 1) == 0);   // sustain (0.6) is one column wide

    // A chord knits stripes: each newly finished row has stitches in all three columns
    tv.noteOn(48, 100); tv.noteOn(72, 100);
    run(kn, tv, 1.0f);
    assert(kn.getRow(0, 1) >= 1 && kn.getRow(0, col) == 1 && kn.getRow(0, 31) >= 1);
    {
        U8G2 d;
        d.begin();
        kn.render(d, tv.ctx);
        assert(blueZoneOnly(d));
        assert(d.getPixel(1, 24) && d.getPixel(0, 23));            // needle bed
        assert(d.getPixel(col * 4, KnitAnimation::TOPY + KnitAnimation::ROWH));   // stitch in row 0
    }

    // Carriage parks after 4 s of quiet, and the next note starts it again
    tv.noteOff(48); tv.noteOff(60); tv.noteOff(72);
    run(kn, tv, 3.0f);
    assert(!kn.isParked());
    run(kn, tv, 2.0f);
    assert(kn.isParked());
    const uint32_t parkedCommits = kn.getCommits();
    run(kn, tv, 2.0f);
    assert(kn.getCommits() == parkedCommits);
    tv.noteOn(64, 100);
    run(kn, tv, 0.1f);
    assert(!kn.isParked());

    kn.reset(); prime(kn);
    assert(kn.isParked() && kn.getRowCount() == 0 && kn.getCommits() == 0);
    std::cout << "  -> KnitAnimation tests PASSED!" << std::endl;
}
