// TypewriterAnimation tests (included by tests/test_toy_animations.cpp)

namespace {
// One short keystroke: press, hold 0.1 s, release, 0.1 s gap (well under the 0.6 s space pause)
void typeKey(TypewriterAnimation& tw, ToyVoices& tv, uint8_t note, uint8_t vel) {
    tv.noteOn(note, vel);
    run(tw, tv, 0.1f);
    tv.noteOff(note);
    run(tw, tv, 0.1f);
}
} // namespace

void test_typewriter() {
    std::cout << "[Test] TypewriterAnimation..." << std::endl;
    TypewriterAnimation tw;
    tw.init(); prime(tw);
    assert(std::strcmp(tw.getName(), "Typewriter") == 0);

    // Hammer row: pivots 8 px apart on the bottom edge, heads leaning in, highest in the middle;
    // low keys swing left hammers, high keys right ones
    assert(TypewriterAnimation::pivotX(0) == 16 && TypewriterAnimation::pivotX(6) == 64);
    assert(TypewriterAnimation::pivotX(TypewriterAnimation::BARS - 1) == 112);
    assert(TypewriterAnimation::restHeadY(6) == 51 && TypewriterAnimation::restHeadY(0) == 54);
    assert(TypewriterAnimation::restHeadX(0) > 16.0f && TypewriterAnimation::restHeadX(12) < 112.0f);
    assert(TypewriterAnimation::barFor(0) == 0 && TypewriterAnimation::barFor(12) == 6);
    assert(TypewriterAnimation::barFor(24) == TypewriterAnimation::BARS - 1);

    // Single notes: one column each, marks set higher for higher keys, ink by velocity
    {
        ToyVoices tv;
        typeKey(tw, tv, 48, 110);
        typeKey(tw, tv, 60, 60);
        typeKey(tw, tv, 72, 20);
        assert(tw.getColumn() == 3);
        assert(tw.getCell(0).count == 1 && tw.getCell(0).rises[0] == 0 && tw.getCell(0).ink == 2);
        assert(tw.getCell(1).count == 1 && tw.getCell(1).rises[0] == 4 && tw.getCell(1).ink == 1);
        assert(tw.getCell(2).count == 1 && tw.getCell(2).rises[0] == 8 && tw.getCell(2).ink == 0);

        // A pause types one space; a 3 s pause returns the carriage
        int steps = 0;
        while (tw.getColumn() == 3 && steps++ < 40) run(tw, tv, 0.05f);
        assert(tw.getColumn() == 4 && tw.getCell(3).count == 0);
        run(tw, tv, 2.7f);
        assert(tw.getColumn() == 0);
        assert(tw.getPreviousLines() == 1 && tw.getPreviousLineLength(0) == 4);
        assert(tw.getPreviousCell(2, 0).rises[0] == 8);

        // Two finished lines stay on the paper, the oldest moving up
        typeKey(tw, tv, 60, 100);
        run(tw, tv, 3.5f);
        assert(tw.getPreviousLines() == 2);
        assert(tw.getPreviousLineLength(0) == 2 && tw.getPreviousLineLength(1) == 4);
        typeKey(tw, tv, 60, 100);
        typeKey(tw, tv, 60, 100);
        run(tw, tv, 3.5f);
        assert(tw.getPreviousLines() == 2);
        assert(tw.getPreviousLineLength(0) == 3 && tw.getPreviousLineLength(1) == 2);
    }

    // A strike flips its hammer up for a moment, then it settles back
    {
        tw.reset(); prime(tw);
        ToyVoices tv;
        U8G2 rest;
        rest.begin();
        tw.render(rest, tv.ctx);
        assert(rest.getPixel(63, 50) && rest.getPixel(65, 51) && rest.getPixel(64, 58)); // middle hammer at rest
        tv.noteOn(60, 100);   // key 12 -> hammer 6
        run(tw, tv, 0.05f);
        assert(tw.isBarSwinging(6) && !tw.isBarSwinging(0));
        U8G2 d;
        d.begin();
        tw.render(d, tv.ctx);
        assert(blueZoneOnly(d));
        assert(!d.getPixel(63, 50) && d.getPixel(64, 58));   // head left its rest spot, arm still there
        run(tw, tv, 0.2f);
        assert(!tw.isBarSwinging(6));
        tv.noteOff(60);
        run(tw, tv, 0.1f);
        // Keys off the keybed pin to the end hammers
        tv.noteOn(30, 100);
        run(tw, tv, 0.05f);
        assert(tw.isBarSwinging(0) && tw.getCell(1).rises[0] == 0);
    }

    // Two-note chord stacks two marks in one column and swings both hammers
    {
        tw.reset(); prime(tw);
        assert(tw.getColumn() == 0 && !tw.hasPreviousLine());
        ToyVoices tv;
        tv.noteOn(48, 100); tv.noteOn(72, 100);
        run(tw, tv, 0.05f);
        assert(tw.isBarSwinging(0) && tw.isBarSwinging(TypewriterAnimation::BARS - 1));
        run(tw, tv, 0.15f);
        assert(tw.getColumn() == 1);
        const auto& c = tw.getCell(0);
        assert(c.count == 2);
        assert((c.rises[0] == 0 && c.rises[1] == 8) || (c.rises[0] == 8 && c.rises[1] == 0));
    }

    // Three-note chord jams: nothing typed, the jammed hammers' heads stay up at the paper
    {
        tw.reset(); prime(tw);
        ToyVoices tv;
        tv.noteOn(48, 100); tv.noteOn(52, 100); tv.noteOn(55, 100);   // keys 0, 4, 7 -> hammers 0, 2, 4
        run(tw, tv, 0.3f);
        assert(tw.isJammed());
        assert(tw.getColumn() == 0);
        assert(!tw.isBarSwinging(0));
        U8G2 d;
        d.begin();
        tw.render(d, tv.ctx);
        assert(blueZoneOnly(d));
        assert(!d.getPixel(25, 53) && d.getPixel(31, 52));  // hammer 0 is up; hammer 1 rests (head 31..33, 52..53)
        // Notes during the jam are swallowed; once it clears typing resumes
        tv.noteOff(48); tv.noteOff(52); tv.noteOff(55);
        run(tw, tv, 0.1f);
        typeKey(tw, tv, 60, 100);
        assert(tw.getColumn() == 0);
        run(tw, tv, 0.5f);
        assert(!tw.isJammed());
        typeKey(tw, tv, 60, 100);
        assert(tw.getColumn() == 1);
    }

    // The bell shakes 4 columns before the margin; a full line returns the carriage
    {
        tw.reset(); prime(tw);
        ToyVoices tv;
        const int bx = TypewriterAnimation::BELL_X, by = TypewriterAnimation::BELL_Y;
        {
            U8G2 d;
            d.begin();
            tw.render(d, tv.ctx);
            assert(d.getPixel(bx + 2, by) && !d.getPixel(bx - 2, by - 1));   // bell at rest, silent
        }
        for (int i = 0; i < TypewriterAnimation::COLS - 4; ++i) typeKey(tw, tv, static_cast<uint8_t>(48 + i), 100);
        assert(tw.getColumn() == TypewriterAnimation::COLS - 4);
        assert(tw.isDinging());
        {
            U8G2 d;
            d.begin();
            tw.render(d, tv.ctx);
            assert(blueZoneOnly(d));
            assert(d.getPixel(bx - 2, by - 1) && d.getPixel(bx + 6, by - 1));
            assert(d.getPixel(bx - 3, by + 1) && d.getPixel(bx + 7, by + 1));
            assert(litPixels(d, bx - 1, by, bx + 5, by + 3) == 12);   // shaken bell sprite, still whole
        }
        for (int i = 0; i < 4; ++i) typeKey(tw, tv, 60, 100);
        assert(tw.getColumn() == 0);
        assert(tw.getPreviousLineLength(0) == TypewriterAnimation::COLS);
        assert(!tw.isDinging());
    }

    // Render: blue zone only; tall paper, platen and a solid mark one cell left of the strike point
    {
        tw.reset(); prime(tw);
        ToyVoices tv;
        typeKey(tw, tv, 48, 110);
        run(tw, tv, 0.3f);   // let the paper ease to its new column
        U8G2 d;
        d.begin();
        tw.render(d, tv.ctx);
        assert(blueZoneOnly(d));
        // Platen roller lines (y45, y48) and paper edge (paperX 60 - 6 = 54, y16..44)
        assert(d.getPixel(70, 45) && d.getPixel(70, 48) && !d.getPixel(70, 46));
        assert(d.getPixel(54, 16) && d.getPixel(54, 44));
        // Column 0 sits one cell left of the strike point: solid 3x3 block, rise 0 (bottom y43)
        assert(d.getPixel(60, 41) && d.getPixel(62, 43) && d.getPixel(61, 42));
    }
    std::cout << "  -> TypewriterAnimation tests PASSED!" << std::endl;
}
