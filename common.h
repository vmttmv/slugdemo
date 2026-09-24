#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define VEC2(x, y)          (vec2){(x), (y)}
#define VEC2S(x, y, upem)   (vec2){(x) / (upem), (y) / (upem)}
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MIN3(a, b, c)       MIN((a), MIN((b), (c)))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define MAX3(a, b, c)       MAX((a), MAX((b), (c)))

typedef struct vec2
{
    float x, y;
} vec2;

typedef struct Curve
{
    uint32_t    index; 
    vec2        min;
    vec2        max;
} Curve;

typedef struct ContourData
{
    uint32_t    point_cap;
    uint32_t    point_count;
    vec2        *points;

    uint32_t    curve_cap;
    uint32_t    curve_count;
    Curve       *curves;

    vec2        min;
    vec2        max;

    // Used in curve extracting
    float       units_per_em;
    vec2        pos;
    bool        is_first;
} ContourData;

typedef struct GlyphBand
{
    uint32_t    count;
    Curve       *curves;
} GlyphBand;

typedef struct Glyph
{
    uint32_t    band_offset;
    uint32_t    band_count;
    vec2        min;
    vec2        max;
    vec2        bearing;
    float       advance;
} Glyph;

typedef struct GPUGlyph
{
    uint32_t    band_offset;
    uint32_t    band_count;
    vec2        min;
    vec2        max;
    vec2        pos;
} GPUGlyph;

static inline vec2 vec2_min(vec2 a, vec2 b)
{
    return VEC2(MIN(a.x, b.x), MIN(a.y, b.y));
}

static inline vec2 vec2_max(vec2 a, vec2 b)
{
    return VEC2(MAX(a.x, b.x), MAX(a.y, b.y));
}

static inline void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

static inline void *xmalloc(size_t size)
{
    void *p = malloc(size);
    if (!p) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    return p;
}

static inline void *xrealloc(void *p, size_t size)
{
    p = realloc(p, size);
    if (!p) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }

    return p;
}
