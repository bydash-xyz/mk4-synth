#pragma once

#include "IAnimation.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32) || defined(MOCK_HAL)

namespace HardwareDriver {
namespace AnimDraw {

// Pixel clipped to the animation zone (x 0..127, y ANIM_REGION_TOP..ANIM_REGION_BOTTOM).
// U8g2 coordinates are unsigned, so callers must never hand it negative or off-screen values.
inline void pixel(U8G2& u8g2, int x, int y) {
    if (x >= 0 && x < SCREEN_WIDTH && y >= ANIM_REGION_TOP && y <= ANIM_REGION_BOTTOM) {
        u8g2.drawPixel(x, y);
    }
}

// Bresenham line, clipped per pixel to the animation zone
inline void line(U8G2& u8g2, int x0, int y0, int x1, int y1) {
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        pixel(u8g2, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Horizontal span x0..x1 (inclusive), clipped to the animation zone
inline void hspan(U8G2& u8g2, int x0, int x1, int y) {
    if (y < ANIM_REGION_TOP || y > ANIM_REGION_BOTTOM) return;
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_WIDTH - 1) x1 = SCREEN_WIDTH - 1;
    if (x0 <= x1) u8g2.drawHLine(x0, y, x1 - x0 + 1);
}

// Vertical span y0..y1 (inclusive), clipped to the animation zone
inline void vspan(U8G2& u8g2, int x, int y0, int y1) {
    for (int y = y0; y <= y1; ++y) pixel(u8g2, x, y);
}

// Outline rectangle, clipped
inline void rect(U8G2& u8g2, int x, int y, int w, int h) {
    hspan(u8g2, x, x + w - 1, y);
    hspan(u8g2, x, x + w - 1, y + h - 1);
    vspan(u8g2, x, y, y + h - 1);
    vspan(u8g2, x + w - 1, y, y + h - 1);
}

// Filled rectangle, clipped
inline void box(U8G2& u8g2, int x, int y, int w, int h) {
    for (int j = 0; j < h; ++j) hspan(u8g2, x, x + w - 1, y + j);
}

// Filled circle, clipped
inline void disc(U8G2& u8g2, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; ++dy) {
        const int h = static_cast<int>(std::sqrt(static_cast<float>(r * r - dy * dy)) + 0.5f);
        hspan(u8g2, cx - h, cx + h, cy + dy);
    }
}

// Midpoint circle outline, clipped. every > 1 plots only every Nth step for a dotted ring.
inline void circle(U8G2& u8g2, int cx, int cy, int r, int every = 1) {
    int x = r, y = 0, err = 1 - r, i = 0;
    while (x >= y) {
        if (i % every == 0) {
            pixel(u8g2, cx + x, cy + y); pixel(u8g2, cx + y, cy + x);
            pixel(u8g2, cx - y, cy + x); pixel(u8g2, cx - x, cy + y);
            pixel(u8g2, cx - x, cy - y); pixel(u8g2, cx - y, cy - x);
            pixel(u8g2, cx + y, cy - x); pixel(u8g2, cx + x, cy - y);
        }
        ++i;
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

// Filled ellipse, clipped
inline void ellipseFill(U8G2& u8g2, int cx, int cy, int rx, int ry) {
    for (int dy = -ry; dy <= ry; ++dy) {
        const float f = 1.0f - static_cast<float>(dy * dy) / static_cast<float>(ry * ry);
        const int h = static_cast<int>(rx * std::sqrt(f > 0.0f ? f : 0.0f) + 0.5f);
        hspan(u8g2, cx - h, cx + h, cy + dy);
    }
}

// Runs a draw call in erase colour (0), e.g. to carve tunnels out of a speckled background
template <typename Fn>
inline void erase(U8G2& u8g2, Fn&& draw) {
    u8g2.setDrawColor(0);
    draw();
    u8g2.setDrawColor(1);
}

// 1-bit sprite from strings, '#' = lit. rows[0] is the top row; all rows share one width.
inline void sprite(U8G2& u8g2, int x, int y, const char* const* rows, int h,
                   bool flipX = false, bool flipY = false) {
    const int w = static_cast<int>(std::strlen(rows[0]));
    for (int j = 0; j < h; ++j) {
        const char* row = rows[flipY ? h - 1 - j : j];
        for (int i = 0; i < w; ++i) {
            if (row[flipX ? w - 1 - i : i] == '#') pixel(u8g2, x + i, y + j);
        }
    }
}

// Tiny 3x5 pixel font for deadpan labels ("DING", "z", "!"). Each glyph is five rows of 3 bits,
// MSB = left column. Covers A-Z, 0-9 and ! ? . : - + / ; anything else draws as '?'.
inline const uint8_t* tinyGlyph(char c) {
    static constexpr uint8_t LETTERS[26][5] = {
        {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},
        {5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{6,5,5,5,5},
        {2,5,5,5,2},{6,5,6,4,4},{2,5,5,6,3},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},{5,5,5,5,7},
        {5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},{7,1,2,4,7},
    };
    static constexpr uint8_t DIGITS[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{6,1,2,4,7},{6,1,2,1,6},{5,5,7,1,1},
        {7,4,6,1,6},{3,4,6,5,2},{7,1,2,2,2},{2,5,2,5,2},{2,5,3,1,6},
    };
    static constexpr uint8_t BANG[5] = {2,2,2,0,2}, QUESTION[5] = {6,1,2,0,2}, DOT[5] = {0,0,0,0,2},
                             COLON[5] = {0,2,0,2,0}, MINUS[5] = {0,0,7,0,0}, PLUS[5] = {0,2,7,2,0},
                             SLASH[5] = {1,1,2,4,4}, SPACE[5] = {0,0,0,0,0};
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return LETTERS[c - 'A'];
    if (c >= '0' && c <= '9') return DIGITS[c - '0'];
    switch (c) {
        case '!': return BANG;
        case '.': return DOT;
        case ':': return COLON;
        case '-': return MINUS;
        case '+': return PLUS;
        case '/': return SLASH;
        case ' ': return SPACE;
        default: return QUESTION;
    }
}

// Draws text in the 3x5 font with its top-left at (x, y); 4 px advance per character
inline void tinyText(U8G2& u8g2, int x, int y, const char* s) {
    for (; *s; ++s, x += 4) {
        const uint8_t* g = tinyGlyph(*s);
        for (int r = 0; r < 5; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (g[r] & (4 >> c)) pixel(u8g2, x + c, y + r);
            }
        }
    }
}

inline int tinyTextWidth(const char* s) {
    const int n = static_cast<int>(std::strlen(s));
    return n > 0 ? n * 4 - 1 : 0;
}

} // namespace AnimDraw
} // namespace HardwareDriver

#endif // ARDUINO_ARCH_ESP32 || MOCK_HAL
