#include "ttf.h"

#include <assert.h>
#include <float.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <freetype/ftoutln.h>

#define TTF_OCCUPANCY_MAX   0.7f

typedef struct TTF_Curve
{
    uint32_t    index;
    TTF_vec2    min;
    TTF_vec2    max;
} TTF_Curve;

typedef struct TTF_Contour
{
    // Data
    uint32_t    point_cap;
    uint32_t    point_count;
    TTF_vec2    *points;

    uint32_t    curve_cap;
    uint32_t    curve_count;
    TTF_Curve   *curves;

    // Bounding box
    TTF_vec2    min;
    TTF_vec2    max;

    // Meta
    float       units_per_em;
    TTF_vec2    pos;
    bool        is_first;
} TTF_Contour;

typedef struct TTF_Band
{
    uint32_t    count;
    TTF_Curve   *curves;
} TTF_Band;

static const TTF_Glyph *ttf_load_glyph(TTF *ttf, uint32_t cp);
static void ttf_push_data(TTF *ttf, const TTF_vec2 *p, uint32_t p_count, const uint32_t *b, uint32_t b_count);

// Cached glyph metadata hash table
static void ttf_lookup_free(TTF_Lookup *l);
static const TTF_Glyph *ttf_lookup_get(const TTF_Lookup *l, uint32_t cp);
static const TTF_Glyph *ttf_lookup_insert(TTF_Lookup *l, const TTF_Glyph *glyph, uint32_t cp);
static void ttf_lookup_grow(TTF_Lookup *l);

// X/Y-band qsort callbacks
static int hband_comp(const void *a, const void *b);
static int vband_comp(const void *a, const void *b);

// Freetype outline processing helpers and callbacks
static void ttf_contour_free(TTF_Contour *c);
static void ttf_contour_push_point(TTF_Contour *c, TTF_vec2 p);
static void ttf_contour_push_curve(TTF_Contour *c, uint32_t index);
static void ttf_contour_push_quadratic(TTF_Contour *c, TTF_vec2 p1, TTF_vec2 p2);

static int ftcb_move_to(const FT_Vector *to, void *user);
static int ftcb_line_to(const FT_Vector *to, void *user);
static int ftcb_conic_to(const FT_Vector *control, const FT_Vector *to, void *user);
static int ftcb_cubic_to(const FT_Vector *control1, const FT_Vector *control2, const FT_Vector *to, void *user);

// libc wrappers
static void ttf_die(const char *msg);
static inline void *ttf_xmalloc(size_t size);
static inline void *ttf_xrealloc(void *p, size_t size);

TTF *ttf_create(const char *path)
{
    TTF *ttf = ttf_xmalloc(sizeof(*ttf));
    memset(ttf, 0, sizeof(*ttf));

    FT_Error error = FT_Init_FreeType(&ttf->ft);
    if (error)
        ttf_die("FT_Init_FreeType");

    error = FT_New_Face(ttf->ft, path, 0, &ttf->face);
    if (error)
        ttf_die("FT_New_Face");

    return ttf;
}

void ttf_destroy(TTF *ttf)
{
    FT_Done_Face(ttf->face);
    FT_Done_FreeType(ttf->ft);
    ttf_lookup_free(&ttf->lookup);
    free(ttf->bands);
    free(ttf->points);
    free(ttf);
}

const TTF_Glyph *ttf_get_glyph(TTF *ttf, uint32_t cp)
{
    const TTF_Glyph *glyph = ttf_lookup_get(&ttf->lookup, cp);
    if (glyph)
        return glyph;

    return ttf_load_glyph(ttf, cp);
}

float ttf_get_height(const TTF *ttf, float size)
{
    float units_per_em = (float)ttf->face->units_per_EM;
    return (float)ttf->face->height / units_per_em * size;
}

const TTF_Glyph *ttf_load_glyph(TTF *ttf, uint32_t cp)
{
    FT_UInt glyph_index = FT_Get_Char_Index(ttf->face, cp);
    FT_Int32 flags = FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM;
    FT_Error error = FT_Load_Glyph(ttf->face, glyph_index, flags);
    if (error)
        ttf_die("FT_Load_Glyph");

    float units_per_em = (float)ttf->face->units_per_EM;
    FT_Outline *outline = &ttf->face->glyph->outline;

    // Flip Y
    FT_Matrix matrix = {0};
    matrix.xx = 0x10000;
    matrix.yy = -0x10000;
    FT_Outline_Transform(outline, &matrix);

    TTF_Contour contour = {0};
    contour.min = TTF_VEC2(FLT_MAX, FLT_MAX);
    contour.max = TTF_VEC2(-FLT_MAX, -FLT_MAX);
    contour.units_per_em = units_per_em;

    FT_Outline_Funcs outline_funcs = {
        .move_to  = ftcb_move_to,
        .line_to  = ftcb_line_to,
        .conic_to = ftcb_conic_to,
        .cubic_to = ftcb_cubic_to
    };
    error = FT_Outline_Decompose(outline, &outline_funcs, &contour);
    if (error)
        ttf_die("FT_Outline_Decompose");

    // Fixed band counts
    uint32_t hband_count = 6;
    uint32_t vband_count = 6;
    float hband_size = (contour.max.y - contour.min.y) / (float)hband_count;
    float vband_size = (contour.max.x - contour.min.x) / (float)vband_count;

    TTF_Band *hbands = ttf_xmalloc(sizeof(*hbands) * hband_count);
    for (uint32_t i = 0; i < hband_count; ++i) {
        hbands[i].count = 0;
        hbands[i].curves = ttf_xmalloc(sizeof(TTF_Curve) * contour.curve_count);
    }

    TTF_Band *vbands = ttf_xmalloc(sizeof(*vbands) * vband_count);
    for (uint32_t i = 0; i < vband_count; ++i) {
        vbands[i].count = 0;
        vbands[i].curves = ttf_xmalloc(sizeof(TTF_Curve) * contour.curve_count);
    }

    // Gather per-band curves
    uint32_t index_count = 0;
    for (uint32_t i = 0; i < contour.curve_count; ++i) {
        const TTF_Curve *curve = contour.curves + i;

        // Horizontal bands
        int h0 = TTF_MAX(0, (int)((curve->min.y - contour.min.y) / hband_size));
        int h1 = TTF_MIN(hband_count - 1, (int)((curve->max.y - contour.min.y) / hband_size));
        for (int j = h0; j <= h1; ++j) {
            TTF_Band *band = hbands + j;
            band->curves[band->count++] = *curve;
            index_count++;
        }

        // Vertical bands
        int v0 = TTF_MAX(0, (int)((curve->min.x - contour.min.x) / vband_size));
        int v1 = TTF_MIN(vband_count - 1, (int)((curve->max.x - contour.min.x) / vband_size));
        for (int j = v0; j <= v1; ++j) {
            TTF_Band *band = vbands + j;
            band->curves[band->count++] = *curve;
            index_count++;
        }
    }

    // Sort curves in bands in descending x (horizontal), and descending y (vertical)
    for (uint32_t i = 0; i < hband_count; ++i)
        qsort(hbands[i].curves, hbands[i].count, sizeof(TTF_Curve), hband_comp);
    for (uint32_t i = 0; i < vband_count; ++i)
        qsort(vbands[i].curves, vbands[i].count, sizeof(TTF_Curve), vband_comp);

    // Write out the band buffer data in the gpu-format
    // First, the headers (struct { uint count, offset; }), 2 x uint32_t
    // followed by the curve indices

    // Number of headers (in multiples of uint32_t), index data starts at this offset
    uint32_t headers_size = (hband_count + vband_count) * 2u;
    uint32_t indices_offset = headers_size;
    uint32_t band_data_count = headers_size + index_count;
    uint32_t *band_data = ttf_xmalloc(sizeof(uint32_t) * band_data_count);

    // Horizontal bands and index data
    for (uint32_t i = 0; i < hband_count; ++i) {
        // Header
        uint32_t header_index = i * 2u;
        band_data[header_index + 0] = hbands[i].count;
        band_data[header_index + 1] = indices_offset;

        // Indices (offset by the # of points already present in the persistent buffer)
        for (uint32_t j = 0; j < hbands[i].count; ++j)
            band_data[indices_offset++] = hbands[i].curves[j].index + ttf->point_count;
    }

    // Vertical bands and index data
    for (uint32_t i = 0; i < vband_count; ++i) {
        // Header
        uint32_t header_index = (hband_count + i) * 2u;
        band_data[header_index + 0] = vbands[i].count;
        band_data[header_index + 1] = indices_offset;

        // Indices (offset by the # of points already present in the persistent buffer)
        for (uint32_t j = 0; j < vbands[i].count; ++j)
            band_data[indices_offset++] = vbands[i].curves[j].index + ttf->point_count;
    }

    uint32_t band_offset = ttf->band_count;
    ttf_push_data(ttf, contour.points, contour.point_count, band_data, band_data_count);

    free(band_data);
    for (uint32_t i = 0; i < vband_count; ++i)
        free(vbands[i].curves);
    free(vbands);
    for (uint32_t i = 0; i < hband_count; ++i)
        free(hbands[i].curves);
    free(hbands);
    ttf_contour_free(&contour);

    const FT_Glyph_Metrics *metrics = &ttf->face->glyph->metrics;

    TTF_Glyph g;
    g.band_offset = band_offset;
    g.band_count = (hband_count << 16) | vband_count;
    g.min = contour.min;
    g.max = contour.max;
    g.bearing.x = metrics->horiBearingX / units_per_em;
    g.bearing.y = metrics->horiBearingY / units_per_em;
    g.advance = metrics->horiAdvance / units_per_em;

    return ttf_lookup_insert(&ttf->lookup, &g, cp);
}

void ttf_push_data(TTF *ttf, const TTF_vec2 *p, uint32_t p_count, const uint32_t *b, uint32_t b_count)
{
    if (ttf->point_count + p_count > ttf->point_cap) {
        ttf->point_cap = TTF_MAX(ttf->point_cap * 2u, ttf->point_count + p_count);
        ttf->points = ttf_xrealloc(ttf->points, ttf->point_cap * sizeof(TTF_vec2));
    }
    memcpy(ttf->points + ttf->point_count, p, p_count * sizeof(TTF_vec2));
    ttf->point_count += p_count;

    if (ttf->band_count + b_count > ttf->band_cap) {
        ttf->band_cap = TTF_MAX(ttf->band_cap * 2u, ttf->band_count + b_count);
        ttf->bands = ttf_xrealloc(ttf->bands, ttf->band_cap * sizeof(uint32_t));
    }
    memcpy(ttf->bands + ttf->band_count, b, b_count * sizeof(uint32_t));
    ttf->band_count += b_count;
    ttf->dirty = 1;
}

void ttf_lookup_free(TTF_Lookup *l)
{
    free(l->vals);
    free(l->keys);
}

const TTF_Glyph *ttf_lookup_get(const TTF_Lookup *l, uint32_t cp)
{
    if (l->count) {
        uint32_t idx = cp % l->cap;
        while (l->keys[idx] != UINT32_MAX) {
            if (l->keys[idx] == cp)
                return l->vals + idx;
            idx = (idx + 1) % l->cap;
        }
    }

    return NULL;
}

const TTF_Glyph *ttf_lookup_insert(TTF_Lookup *l, const TTF_Glyph *glyph, uint32_t cp)
{
    if (l->cap == 0) {
        ttf_lookup_grow(l);
    } else {
        float occupancy = (float)l->count / (float)l->cap;
        if (occupancy >= TTF_OCCUPANCY_MAX)
            ttf_lookup_grow(l);
    }

    uint32_t idx = cp % l->cap;
    while (l->keys[idx] != UINT32_MAX) {
        // Duplicate insertion?
        if (l->keys[idx] == cp)
            break;

        idx = (idx + 1) % l->cap;
    }

    l->keys[idx] = cp;
    l->vals[idx] = *glyph;
    l->count++;
    return l->vals + idx;
}

void ttf_lookup_grow(TTF_Lookup *l)
{
    uint32_t new_cap = TTF_MAX(l->cap * 2u, 128u);
    size_t vals_size = new_cap * sizeof(TTF_Glyph);
    size_t keys_size = new_cap * sizeof(uint32_t);

    TTF_Glyph *new_vals = ttf_xmalloc(vals_size);
    uint32_t *new_keys = ttf_xmalloc(keys_size);
    memset(new_keys, 0xff, keys_size);

    for (uint32_t i = 0, j = 0; i < l->cap && j < l->count; ++i) {
        if (l->keys[i] == UINT32_MAX)
            continue;

        uint32_t key = l->keys[i];
        uint32_t idx = key % new_cap;
        while (new_keys[idx] != UINT32_MAX)
            idx = (idx + 1) % new_cap;
        new_keys[idx] = key;
        new_vals[idx] = l->vals[i];
        j++;
    }

    free(l->vals);
    free(l->keys);

    l->vals = new_vals;
    l->keys = new_keys;
    l->cap = new_cap;
}

int hband_comp(const void *a, const void *b)
{
    const TTF_Curve *ca = a;
    const TTF_Curve *cb = b;
    return (ca->max.x < cb->max.x) - (ca->max.x > cb->max.x);
}

int vband_comp(const void *a, const void *b)
{
    const TTF_Curve *ca = a;
    const TTF_Curve *cb = b;
    return (ca->max.y < cb->max.y) - (ca->max.y > cb->max.y);
}

void ttf_contour_free(TTF_Contour *c)
{
    free(c->points);
    free(c->curves);
}

void ttf_contour_push_point(TTF_Contour *c, TTF_vec2 p)
{
    if (c->point_count >= c->point_cap) {
        c->point_cap = c->point_cap == 0 ? 128 : c->point_cap * 2;
        c->points = ttf_xrealloc(c->points, c->point_cap * sizeof(TTF_vec2));
    }
    c->points[c->point_count++] = p;
}

void ttf_contour_push_curve(TTF_Contour *c, uint32_t index)
{
    assert(index + 2 < c->point_count);

    const TTF_vec2 *p0 = c->points + index;
    const TTF_vec2 *p1 = c->points + index + 1;
    const TTF_vec2 *p2 = c->points + index + 2;

    TTF_Curve curve;
    curve.index = index;
    curve.min.x = TTF_MIN3(p0->x, p1->x, p2->x);
    curve.min.y = TTF_MIN3(p0->y, p1->y, p2->y);
    curve.max.x = TTF_MAX3(p0->x, p1->x, p2->x);
    curve.max.y = TTF_MAX3(p0->y, p1->y, p2->y);

    c->min = TTF_VEC2_MIN(c->min, curve.min);
    c->max = TTF_VEC2_MAX(c->max, curve.max);

    if (c->curve_count >= c->curve_cap) {
        c->curve_cap = c->curve_cap== 0 ? 128 : c->curve_cap * 2;
        c->curves = ttf_xrealloc(c->curves, c->curve_cap * sizeof(TTF_Curve));
    }
    c->curves[c->curve_count++] = curve;
}

void ttf_contour_push_quadratic(TTF_Contour *c, TTF_vec2 p1, TTF_vec2 p2)
{
    if (c->is_first) {
        ttf_contour_push_point(c, c->pos);
        c->is_first = false;
    }

    uint32_t start_index = (uint32_t)(c->point_count - 1);
    ttf_contour_push_point(c, p1);
    ttf_contour_push_point(c, p2);
    ttf_contour_push_curve(c, start_index);

    c->pos = p2;
}

int ftcb_move_to(const FT_Vector *to, void *user)
{
    TTF_Contour *c = user;
    c->pos = TTF_VEC2S((float)to->x, (float)to->y, c->units_per_em);
    c->is_first = true;
    return 0;
}

int ftcb_line_to(const FT_Vector *to, void *user)
{
    TTF_Contour *c = user;
    TTF_vec2 end = TTF_VEC2S((float)to->x, (float)to->y, c->units_per_em);
    ttf_contour_push_quadratic(c, end, end);
    return 0;
}

static int ftcb_conic_to(const FT_Vector *control, const FT_Vector *to, void *user)
{
    TTF_Contour *c = user;
    TTF_vec2 ctrl = TTF_VEC2S((float)control->x, (float)control->y, c->units_per_em);
    TTF_vec2 end  = TTF_VEC2S((float)to->x, (float)to->y, c->units_per_em);
    ttf_contour_push_quadratic(c, ctrl, end);
    return 0;
}

static int ftcb_cubic_to(const FT_Vector *control1, const FT_Vector *control2, const FT_Vector *to, void *user)
{
    TTF_Contour *c = user;
    TTF_vec2 p0 = c->pos;
    TTF_vec2 p1 = TTF_VEC2S((float)control1->x, (float)control1->y, c->units_per_em);
    TTF_vec2 p2 = TTF_VEC2S((float)control2->x, (float)control2->y, c->units_per_em);
    TTF_vec2 p3 = TTF_VEC2S((float)to->x, (float)to->y, c->units_per_em);
    TTF_vec2 q1 = TTF_VEC2((p0.x + 3.0f * p1.x) / 4.0f, (p0.y + 3.0f * p1.y) / 4.0f);
    TTF_vec2 q2 = TTF_VEC2((p3.x + 3.0f * p2.x) / 4.0f, (p3.y + 3.0f * p2.y) / 4.0f);
    TTF_vec2 m  = TTF_VEC2((q1.x + q2.x) / 2.0f, (q1.y + q2.y) / 2.0f);

    ttf_contour_push_quadratic(c, q1, m);
    ttf_contour_push_quadratic(c, q2, p3);
    
    return 0;
}

void ttf_die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

void *ttf_xmalloc(size_t size)
{
    void *p = malloc(size);
    if (!p) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    return p;
}

void *ttf_xrealloc(void *p, size_t size)
{
    p = realloc(p, size);
    if (!p) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }

    return p;
}
