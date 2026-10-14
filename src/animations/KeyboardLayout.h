#pragma once

namespace HardwareDriver {
namespace KeyboardLayout {

// Akai MPK mini MK4 keybed: 25 keys, C to C. At the centre octave with no transpose the
// leftmost key sends MIDI note 48 (C3) and the rightmost sends 72 (C5).
constexpr int KEY_COUNT  = 25;
constexpr int BASE_NOTE  = 48;
constexpr int WHITE_KEYS = 15;

// Maps a received MIDI note to the physical key (0..24), or -1 if it is off the keybed.
// noteOffset is the shift the MPK applied itself (octave/transpose, in semitones).
constexpr int keyIndexForNote(int note, int noteOffset) {
    const int key = note - noteOffset - BASE_NOTE;
    return (key >= 0 && key < KEY_COUNT) ? key : -1;
}

constexpr bool isBlackKey(int key) {
    const int pc = key % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// Index (0..14) of the white key column a key belongs to. For a black key it is the white
// column immediately to its left, so the black key straddles columns [c] and [c + 1].
constexpr int whiteColumn(int key) {
    constexpr int PC_TO_COL[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    return (key / 12) * 7 + PC_TO_COL[key % 12];
}

// --- Pixel geometry inside the 128x64 blue zone (y 16..62) ---------------------------------
// White keys share their borders: 15 columns 8 px apart, each frame 9 px wide.
constexpr int KEY_LEFT   = 3;
constexpr int KEY_TOP    = 18;
constexpr int WHITE_W    = 9;
constexpr int WHITE_STEP = 8;
constexpr int WHITE_H    = 43;   // y 18..60
constexpr int BLACK_W    = 5;
constexpr int BLACK_H    = 27;

constexpr int whiteKeyX(int column) { return KEY_LEFT + column * WHITE_STEP; }

// Black keys are centred on the border between white column c and c + 1.
constexpr int blackKeyX(int key) { return whiteKeyX(whiteColumn(key) + 1) - BLACK_W / 2; }

} // namespace KeyboardLayout
} // namespace HardwareDriver
