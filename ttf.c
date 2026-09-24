#include <ft2build.h>
#include FT_FREETYPE_H
#include <freetype/ftoutln.h>

#include <float.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"

#define POINTS_PER_GLYPH_MAX    (1 << 12)
#define TTF_POINT_BUFFER_MAX    (1 << 18)
#define TTF_BAND_BUFFER_MAX     (1 << 18)
#define TTF_LUT_SIZE            1024

static FT_Library ft_lib;
static FT_Face ft_face;

static uint32_t ttf_point_buffer_offset;
static vec2 ttf_point_buffer[TTF_POINT_BUFFER_MAX];
static uint32_t ttf_band_buffer_offset;
static uint32_t ttf_band_buffer[TTF_BAND_BUFFER_MAX];
static bool ttf_buffers_dirty;

// LUT
static uint32_t lut_count;
static uint32_t lut_keys[TTF_LUT_SIZE];
static Glyph lut_vals[TTF_LUT_SIZE];

static int glyph_hband_comp(const void *a, const void *b)
{
    const Curve *ca = a;
    const Curve *cb = b;
    return (ca->max.x < cb->max.x) - (ca->max.x > cb->max.x);
}

static int glyph_vband_comp(const void *a, const void *b)
{
    const Curve *ca = a;
    const Curve *cb = b;
    return (ca->max.y < cb->max.y) - (ca->max.y > cb->max.y);
}

static void push_point(ContourData *d, vec2 p)
{
    if (d->point_count >= d->point_cap) {
        d->point_cap = d->point_cap == 0 ? 128 : d->point_cap * 2;
        d->points = xrealloc(d->points, d->point_cap * sizeof(vec2));
    }
    d->points[d->point_count++] = p;
}

static void push_curve(ContourData *d, uint32_t index)
{
    assert(index + 2 < d->point_count);
    const vec2 *p0 = d->points + index;
    const vec2 *p1 = d->points + index + 1;
    const vec2 *p2 = d->points + index + 2;

    Curve curve;
    curve.index = index;
    curve.min.x = MIN3(p0->x, p1->x, p2->x);
    curve.min.y = MIN3(p0->y, p1->y, p2->y);
    curve.max.x = MAX3(p0->x, p1->x, p2->x);
    curve.max.y = MAX3(p0->y, p1->y, p2->y);

    d->min = vec2_min(d->min, curve.min);
    d->max = vec2_max(d->max, curve.max);

    if (d->curve_count >= d->curve_cap) {
        d->curve_cap = d->curve_cap== 0 ? 128 : d->curve_cap * 2;
        d->curves = xrealloc(d->curves, d->curve_cap * sizeof(Curve));
    }
    d->curves[d->curve_count++] = curve;
}

static void add_quadratic(ContourData *d, vec2 p1, vec2 p2)
{
    if (d->is_first) {
        push_point(d, d->pos);
        d->is_first = false;
    }

    uint32_t start_index = (uint32_t)(d->point_count - 1);
    push_point(d, p1);
    push_point(d, p2);
    push_curve(d, start_index);

    d->pos = p2;
}

// --- FreeType Callbacks ---
static int cb_move_to(const FT_Vector *to, void* user)
{
    ContourData *d = user;
    d->pos = VEC2S((float)to->x, (float)to->y, d->units_per_em);
    d->is_first = true; // A new contour is starting

    return 0;
}

static int cb_line_to(const FT_Vector *to, void* user)
{
    ContourData *d = user;
    vec2 end = VEC2S((float)to->x, (float)to->y, d->units_per_em);
    add_quadratic(d, end, end);

    return 0;
}

static int cb_conic_to(const FT_Vector *control, const FT_Vector* to, void* user)
{
    ContourData *d = user;
    vec2 ctrl = VEC2S((float)control->x, (float)control->y, d->units_per_em);
    vec2 end  = VEC2S((float)to->x, (float)to->y, d->units_per_em);
    add_quadratic(d, ctrl, end);

    return 0;
}

static int cb_cubic_to(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector *to, void *user)
{
    ContourData *d = user;
    vec2 p0 = d->pos;
    vec2 p1 = VEC2S((float)control1->x, (float)control1->y, d->units_per_em);
    vec2 p2 = VEC2S((float)control2->x, (float)control2->y, d->units_per_em);
    vec2 p3 = VEC2S((float)to->x, (float)to->y, d->units_per_em);
    vec2 q1 = VEC2((p0.x + 3.0f * p1.x) / 4.0f, (p0.y + 3.0f * p1.y) / 4.0f);
    vec2 q2 = VEC2((p3.x + 3.0f * p2.x) / 4.0f, (p3.y + 3.0f * p2.y) / 4.0f);
    vec2 m  = VEC2((q1.x + q2.x) / 2.0f, (q1.y + q2.y) / 2.0f);

    add_quadratic(d, q1, m);
    add_quadratic(d, q2, p3);
    
    return 0;
}

static void extract_contours(ContourData *result, FT_Outline *outline)
{
    FT_Outline_Funcs outline_funcs = {
        .move_to  = cb_move_to,
        .line_to  = cb_line_to,
        .conic_to = cb_conic_to,
        .cubic_to = cb_cubic_to,
        .shift    = 0,
        .delta    = 0
    };

    FT_Error error = FT_Outline_Decompose(outline, &outline_funcs, result);
    if (error)
        die("FT_Outline_Decompose");
}

void ttf_init(const char *font_path)
{
    FT_Error error = FT_Init_FreeType(&ft_lib);
    if (error)
        die("FT_Init_FreeType");

    error = FT_New_Face(ft_lib, font_path, 0, &ft_face);
    if (error)
        die("FT_New_Face");

    memset(lut_keys, 0xff, sizeof(lut_keys));
}

const Glyph *ttf_load_glyph(uint32_t cp)
{
    FT_UInt glyph_index = FT_Get_Char_Index(ft_face, cp);
    FT_Int32 flags = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM;
    FT_Error error = FT_Load_Glyph(ft_face, glyph_index, flags);
    if (error)
        die("FT_Load_Glyph");

    float units_per_em = (float)ft_face->units_per_EM;
    FT_Outline *outline = &ft_face->glyph->outline;

    // Flip Y
    FT_Matrix matrix = {0};
    matrix.xx = 0x10000;
    matrix.yy = -0x10000;
    FT_Outline_Transform(outline, &matrix);

    ContourData data = {0};
    data.min = VEC2(FLT_MAX, FLT_MAX);
    data.max = VEC2(-FLT_MAX, -FLT_MAX);
    data.units_per_em = units_per_em;
    extract_contours(&data, outline);

    uint32_t hband_count = 6;
    uint32_t vband_count = 6;
    float hband_size = (data.max.y - data.min.y) / (float)hband_count;
    float vband_size = (data.max.x - data.min.x) / (float)vband_count;

    GlyphBand *hbands = xmalloc(sizeof(*hbands) * hband_count);
    for (uint32_t i = 0; i < hband_count; ++i) {
        hbands[i].count = 0;
        hbands[i].curves = xmalloc(sizeof(Curve) * data.curve_count);
    }

    GlyphBand *vbands = xmalloc(sizeof(*vbands) * vband_count);
    for (uint32_t i = 0; i < vband_count; ++i) {
        vbands[i].count = 0;
        vbands[i].curves = xmalloc(sizeof(Curve) * data.curve_count);
    }

    uint32_t index_count = 0;
    for (uint32_t i = 0; i < data.curve_count; ++i) {
        const Curve *curve = data.curves + i;

        int h0 = MAX(0, (int)((curve->min.y - data.min.y) / hband_size));
        int h1 = MIN(hband_count - 1, (int)((curve->max.y - data.min.y) / hband_size));
        for (int j = h0; j <= h1; ++j) {
            GlyphBand *band = hbands + j;
            band->curves[band->count++] = *curve;
            index_count++;
        }

        int v0 = MAX(0, (int)((curve->min.x - data.min.x) / vband_size));
        int v1 = MIN(vband_count - 1, (int)((curve->max.x - data.min.x) / vband_size));
        for (int j = v0; j <= v1; ++j) {
            GlyphBand *band = vbands + j;
            band->curves[band->count++] = *curve;
            index_count++;
        }
    }

    for (uint32_t i = 0; i < hband_count; ++i)
        qsort(hbands[i].curves, hbands[i].count, sizeof(Curve), glyph_hband_comp);
    for (uint32_t i = 0; i < vband_count; ++i)
        qsort(vbands[i].curves, vbands[i].count, sizeof(Curve), glyph_vband_comp);

    uint32_t headers_size = (hband_count + vband_count) * 2u;
    uint32_t indices_offset = headers_size;
    uint32_t band_data_count = headers_size + index_count;
    uint32_t *band_data = xmalloc(sizeof(uint32_t) * band_data_count);

    for (uint32_t i = 0; i < hband_count; ++i) {
        uint32_t header_index = i * 2u;
        band_data[header_index + 0] = hbands[i].count;
        band_data[header_index + 1] = indices_offset;

        for (uint32_t j = 0; j < hbands[i].count; ++j)
            band_data[indices_offset++] = hbands[i].curves[j].index + ttf_point_buffer_offset;
    }

    for (uint32_t i = 0; i < vband_count; ++i) {
        uint32_t header_index = (hband_count + i) * 2u;
        band_data[header_index + 0] = vbands[i].count;
        band_data[header_index + 1] = indices_offset;

        for (uint32_t j = 0; j < vbands[i].count; ++j)
            band_data[indices_offset++] = vbands[i].curves[j].index + ttf_point_buffer_offset;
    }

    uint32_t band_offset = ttf_band_buffer_offset;
    if (ttf_point_buffer_offset + data.point_count > TTF_POINT_BUFFER_MAX)
        die("point buffer full");
    if (ttf_band_buffer_offset + band_data_count > TTF_BAND_BUFFER_MAX)
        die("band buffer full");
    memcpy(ttf_point_buffer + ttf_point_buffer_offset, data.points, sizeof(*data.points) * data.point_count);
    ttf_point_buffer_offset += data.point_count;
    memcpy(ttf_band_buffer + ttf_band_buffer_offset, band_data, sizeof(*band_data) * band_data_count);
    ttf_band_buffer_offset += band_data_count;
    ttf_buffers_dirty = true;

    for (uint32_t i = 0; i < vband_count; ++i)
        free(vbands[i].curves);
    free(vbands);
    for (uint32_t i = 0; i < hband_count; ++i)
        free(hbands[i].curves);
    free(hbands);
    free(data.curves);
    free(data.points);

    uint32_t idx = cp % TTF_LUT_SIZE;
    while (lut_keys[idx] != UINT32_MAX)
        idx = (idx + 1) % TTF_LUT_SIZE;
    lut_count++;
    lut_keys[idx] = cp;

    Glyph *glyph = lut_vals + idx;
    glyph->band_offset = band_offset;
    glyph->band_count = (hband_count << 16) | vband_count;
    glyph->min = data.min;
    glyph->max = data.max;

    const FT_Glyph_Metrics *metrics = &ft_face->glyph->metrics;
    glyph->bearing.x = metrics->horiBearingX / units_per_em;
    glyph->bearing.y = metrics->horiBearingY / units_per_em;
    glyph->advance = metrics->horiAdvance / units_per_em;

    return glyph;
}

const Glyph *ttf_get_glyph(uint32_t cp)
{
    uint32_t idx = cp % TTF_LUT_SIZE;
    while (lut_keys[idx] != UINT32_MAX) {
        if (lut_keys[idx] == cp)
            return lut_vals + idx;
        idx = (idx + 1) % TTF_LUT_SIZE;
    }

    return ttf_load_glyph(cp);
}

float ttf_get_font_height(float size)
{
    float units_per_em = (float)ft_face->units_per_EM;
    return (float)ft_face->height / units_per_em * size;
}
