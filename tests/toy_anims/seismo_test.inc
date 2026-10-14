// SeismoAnimation tests (included by tests/test_toy_animations.cpp)

namespace {

// Peak |deflection| over `seconds` after one note at `vel`
float seismoPeak(uint8_t vel) {
    SeismoAnimation anim;
    anim.init(); prime(anim);
    ToyVoices tv;
    run(anim, tv, 0.5f);
    tv.noteOn(60, vel);
    float peak = 0.0f;
    for (int f = 0; f < 10; ++f) {
        run(anim, tv, 0.05f);
        if (f == 1) tv.noteOff(60);
        peak = std::fabs(anim.getDeflection()) > peak ? std::fabs(anim.getDeflection()) : peak;
    }
    return peak;
}

} // namespace

void test_seismo() {
    std::cout << "[Test] SeismoAnimation..." << std::endl;
    SeismoAnimation anim;
    anim.init(); prime(anim);
    assert(std::strcmp(anim.getName(), "Seismograph") == 0);
    ToyVoices tv;

    // Paper scrolls 16 px per beat: 120 BPM -> 32 columns/s
    run(anim, tv, 2.0f);
    assert(anim.getColumnsScrolled() >= 63 && anim.getColumnsScrolled() <= 64);
    tv.ctx.bpm = 60.0f;
    const uint32_t before = anim.getColumnsScrolled();
    run(anim, tv, 2.0f);
    assert(anim.getColumnsScrolled() - before >= 31 && anim.getColumnsScrolled() - before <= 33);
    tv.ctx.bpm = 120.0f;

    // Beat marks every 16 columns, every 4th one denser (downbeat)
    run(anim, tv, 4.0f);
    int prev = -1, beats = 0, downs = 0;
    for (int x = 0; x < SeismoAnimation::HIST; ++x) {
        const uint8_t m = anim.getMark(x);
        if (!m) continue;
        if (prev >= 0) assert(x - prev >= 15 && x - prev <= 17);
        prev = x;
        ++beats;
        if (m == 2) ++downs;
    }
    assert(beats >= 5 && beats <= 6);
    assert(downs >= 1 && downs <= 2);

    // Quiet paper: flat trace
    for (int x = 0; x < SeismoAnimation::HIST; ++x) assert(std::fabs(anim.getHistory(x)) < 0.01f);

    // A hard hit kicks harder than a soft one
    const float hard = seismoPeak(127), soft = seismoPeak(20);
    assert(hard > soft * 1.5f);
    assert(soft > 2.0f);

    // ... and it rings down
    tv.noteOn(60, 127);
    run(anim, tv, 0.1f);
    tv.noteOff(60);
    assert(anim.getJolt() > 0.0f);
    run(anim, tv, 0.1f);
    float peak = 0.0f;
    for (int x = 0; x < SeismoAnimation::HIST; ++x) peak = std::fabs(anim.getHistory(x)) > peak ? std::fabs(anim.getHistory(x)) : peak;
    assert(peak > 10.0f);
    run(anim, tv, 3.0f);
    assert(std::fabs(anim.getDeflection()) < 0.5f);
    assert(anim.getJolt() == 0.0f);

    // A held note adds a tremor; low notes swing wider than high ones
    auto tremorPeak = [](uint8_t note) {
        SeismoAnimation a;
        a.init(); prime(a);
        ToyVoices t;
        t.noteOn(note, 100);
        run(a, t, 1.0f);           // let the strike ring down; tremor remains
        float p = 0.0f;
        for (int f = 0; f < 40; ++f) {
            run(a, t, 0.05f);
            const float tr = std::fabs(a.getTrace() - a.getDeflection());
            p = tr > p ? tr : p;
        }
        return p;
    };
    const float low = tremorPeak(48), high = tremorPeak(72);
    assert(low > 2.5f && high > 0.5f && low > high * 2.0f);

    // Render: blue zone only, paper edge, housing, and the downbeat column denser than a beat
    U8G2 d;
    d.begin();
    anim.render(d, tv.ctx);
    assert(blueZoneOnly(d));
    assert(d.getPixel(0, 18) && d.getPixel(0, 61));
    assert(d.getPixel(99, 30));                       // paper roll
    assert(d.getPixel(127, 30));                      // housing wall
    assert(d.getPixel(SeismoAnimation::PEN_X, SeismoAnimation::MID)); // pen at rest
    for (int x = 0; x < SeismoAnimation::HIST; ++x) {
        if (anim.getMark(x) == 2) assert(d.getPixel(x, 22) && d.getPixel(x, 26));
        if (anim.getMark(x) == 1) assert(!d.getPixel(x, 22) && d.getPixel(x, 24));
    }

    anim.reset(); prime(anim);
    assert(anim.getColumnsScrolled() == 0 && anim.getDeflection() == 0.0f);
    std::cout << "  -> SeismoAnimation tests PASSED!" << std::endl;
}
