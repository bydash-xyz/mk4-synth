// Pond: spring-mesh water and a wave-riding duck (included by tests/test_toy_animations.cpp)

void test_pond() {
    std::cout << "[Test] PondAnimation..." << std::endl;
    PondAnimation anim;
    anim.init(); prime(anim);
    assert(std::strcmp(anim.getName(), "Pond") == 0);
    ToyVoices tv;
    run(anim, tv, 0.2f);
    for (int i = 0; i < PondAnimation::N; ++i) assert(anim.getNodeHeight(i) == 0.0f);
    assert(std::fabs(anim.surfaceAt(30.0f) - PondAnimation::REST) < 1e-4f);

    // A low note pokes the surface near its noteToX spot (left side), not the far right,
    // throws up droplets and sends the duck paddling that way
    tv.noteOn(50, 100);
    run(anim, tv, 0.05f);
    const float x = AnimUtil::noteToX(50, tv.ctx);
    const int node = static_cast<int>(x / 2.0f + 0.5f);
    assert(anim.getNodeHeight(node) > 1.0f);                       // pushed down
    assert(std::fabs(anim.getNodeHeight(PondAnimation::N - 3)) < 0.05f);
    assert(anim.getDropCount() >= 2);
    assert(std::fabs(anim.getDuckTarget() - x) < 1e-4f);
    const float duckX0 = anim.getDuckX();
    run(anim, tv, 1.0f);
    assert(anim.getDuckX() < duckX0 - 10.0f);
    tv.noteOff(50);
    run(anim, tv, 2.0f);
    assert(anim.getDropCount() == 0); // all splashed back down

    U8G2 d;
    d.begin();
    anim.render(d, tv.ctx);
    assert(blueZoneOnly(d));
    assert(d.getPixel(100, static_cast<int>(std::floor(anim.surfaceAt(100.0f) + 0.5f)))); // water line
    assert(litPixels(d, 0, 50, 127, 61) > 20);                                              // water dots

    // A single hard hit right under the duck launches it into a backflip, and it lands again
    {
        PondAnimation pond;
        pond.init(); prime(pond);
        ToyVoices t2;
        run(pond, t2, 0.1f);
        assert(std::fabs(AnimUtil::noteToX(60, t2.ctx) - pond.getDuckX()) < 1.0f); // key 60 is under the duck
        t2.noteOn(60, 127);
        run(pond, t2, 0.05f);
        assert(pond.isDuckAirborne() && pond.isDuckFlipping());
        const float launchY = pond.getDuckY();
        run(pond, t2, 0.1f);
        assert(pond.isDuckAirborne() && pond.getDuckY() < launchY); // rising
        U8G2 d2;
        d2.begin();
        pond.render(d2, t2.ctx);
        assert(blueZoneOnly(d2));
        assert(litPixels(d2, static_cast<int>(pond.getDuckX()) - 6, 16, static_cast<int>(pond.getDuckX()) + 6, 40) > 40); // the big duck
        t2.noteOff(60);
        run(pond, t2, 3.0f);
        assert(!pond.isDuckAirborne());

        // A medium hit launches it without the flip
        PondAnimation hop;
        hop.init(); prime(hop);
        ToyVoices t4;
        run(hop, t4, 0.1f);
        t4.noteOn(60, 90);
        run(hop, t4, 0.05f);
        assert(hop.isDuckAirborne() && !hop.isDuckFlipping());

        // A soft note far away doesn't launch it
        PondAnimation calm;
        calm.init(); prime(calm);
        ToyVoices t3;
        t3.noteOn(72, 20);
        for (int i = 0; i < 20; ++i) { run(calm, t3, 0.05f); assert(!calm.isDuckAirborne()); }
    }

    // Quacks on the downbeat only; dozes off after 6 s of silence
    anim.onBeat(1, false);
    assert(!anim.isQuacking());
    anim.onBeat(0, true);
    assert(anim.isQuacking());
    run(anim, tv, 0.25f);
    assert(!anim.isQuacking());
    run(anim, tv, 5.0f);
    assert(anim.isAsleep() && anim.getZCount() > 0);
    d.clearBuffer();
    anim.render(d, tv.ctx);
    assert(blueZoneOnly(d));

    anim.reset(); prime(anim);
    assert(!anim.isAsleep() && anim.getDropCount() == 0 && anim.getDuckX() == 64.0f);
    std::cout << "  -> PondAnimation tests PASSED!" << std::endl;
}
