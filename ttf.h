#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdint.h>

#define TTF_MIN(a, b)       ((a)<(b) ? (a) : (b))
#define TTF_MIN3(a, b, c)   TTF_MIN((a), TTF_MIN((b), (c)))
#define TTF_MAX(a, b)       ((a)>(b) ? (a) : (b))
#define TTF_MAX3(a, b, c)   TTF_MAX((a), TTF_MAX((b), (c)))

#define TTF_VEC2(x, y)      (TTF_vec2){(x), (y)}
#define TTF_VEC2S(x, y, s)  (TTF_vec2){(x)/(s), (y)/(s)}
#define TTF_VEC2_MIN(a, b)  TTF_VEC2(TTF_MIN((a).x, (b).x), TTF_MIN((a).y, (b).y))
#define TTF_VEC2_MAX(a, b)  TTF_VEC2(TTF_MAX((a).x, (b).x), TTF_MAX((a).y, (b).y))

typedef struct TTF_vec2
{
    float x, y;
} TTF_vec2;

typedef struct TTF_Glyph
{
    uint32_t    band_offset;
    uint32_t    band_count;
    TTF_vec2    min;
    TTF_vec2    max;
    TTF_vec2    bearing;
    float       advance;
} TTF_Glyph;

typedef struct TTF_Lookup
{
    uint32_t    cap;
    uint32_t    count;
    uint32_t    *keys;
    TTF_Glyph   *vals;
} TTF_Lookup;

typedef struct TTF
{
    FT_Library  ft;
    FT_Face     face;
    TTF_Lookup  lookup;

    uint32_t    point_cap;
    uint32_t    point_count;
    TTF_vec2    *points;

    uint32_t    band_cap;
    uint32_t    band_count;
    uint32_t    *bands;

    int         dirty;
} TTF;

TTF *ttf_create(const char *path);
void ttf_destroy(TTF *ttf);
const TTF_Glyph *ttf_get_glyph(TTF *ttf, uint32_t cp);
float ttf_get_height(const TTF *ttf, float size);
