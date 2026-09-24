#version 460
#extension GL_EXT_buffer_reference : require

struct Glyph
{
    uint    band_offset;
    uint    band_count;
    vec2    min;
    vec2    max;
    vec2    pos;
};

layout(std430, buffer_reference, buffer_reference_align = 4) readonly buffer b_bands { uint data[]; };
layout(std430, buffer_reference, buffer_reference_align = 8) readonly buffer b_points { vec2 data[]; };
layout(std430, buffer_reference, buffer_reference_align = 8) readonly buffer b_glyphs { Glyph data[]; };

layout(push_constant) uniform u_constants
{
    vec2        view;
    uint        color;
    float       size;
    b_bands     band_ptr;
    b_points    point_ptr;
    b_glyphs    glyph_ptr;
} pc;

#ifdef VERTEX_SHADER
layout(location = 0) out b_data
{
    vec2        uv;
    flat vec4   color;
    flat vec2   min;
    flat vec2   max;
    flat uint   band_offset;
    flat uint   hband_count;
    flat uint   vband_count;
} vs;

const vec2[4] LUT = vec2[4](
    vec2(0.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(1.0, 0.0)
);

vec4 srgb_to_linear(vec4 c)
{
    bvec3 cutoff = lessThanEqual(c.rgb, vec3(0.04045));
    vec3 lower = c.rgb / 12.92;
    vec3 higher = pow((c.rgb + 0.055) / 1.055, vec3(2.4));
    return vec4(mix(higher, lower, cutoff), c.a);
}

vec4 unpack_rgba(uint c)
{
    float r = ((c >> 24) & 0xff) / 255.0;
    float g = ((c >> 16) & 0xff) / 255.0;
    float b = ((c >> 8)  & 0xff) / 255.0;
    float a = (c         & 0xff) / 255.0;
    return srgb_to_linear(vec4(r, g, b, a));
}

void main(void)
{
    Glyph g = pc.glyph_ptr.data[gl_InstanceIndex];

    vec2 size = g.max - g.min;
    vec2 w = LUT[gl_VertexIndex];
    vec2 uv = g.min + size*w;
    vec2 pos = g.pos + (uv - g.min) * pc.size;

    const float dilation = 0.5;
    vec2 d = (2.0 * w - 1.0) * dilation;

    pos += d;
    uv += d / pc.size;

    gl_Position = vec4(2.0*pos / pc.view - 1.0, 0.0, 1.0);

    vs.uv = uv;
    vs.color = unpack_rgba(pc.color);
    vs.min = g.min;
    vs.max = g.max;
    vs.hband_count = g.band_count >> 16;
    vs.vband_count = g.band_count & 0xffff;
    vs.band_offset = g.band_offset;
}
#endif // VERTEX_SHADER

#ifdef FRAGMENT_SHADER
layout(location = 0) in b_data
{
    vec2        uv;
    flat vec4   color;
    flat vec2   min;
    flat vec2   max;
    flat uint   band_offset;
    flat uint   hband_count;
    flat uint   vband_count;
} fs;
layout(location = 0) out vec4 frag_color;

uint calc_root_code(float y1, float y2, float y3)
{
	// Calculate the root eligibility code for a sample-relative quadratic Bézier curve.
	// Extract the signs of the y coordinates of the three control points.
	uint i1 = floatBitsToUint(y1) >> 31u;
	uint i2 = floatBitsToUint(y2) >> 30u;
	uint i3 = floatBitsToUint(y3) >> 29u;

	uint shift = (i2 & 2u) | (i1 & ~2u);
	shift = (i3 & 4u) | (shift & ~4u);

	// Eligibility is returned in bits 0 and 8.
	return (0x2e74u >> shift) & 0x0101u;
}

vec2 solve_horiz_poly(vec4 p12, vec2 p3)
{
	// Solve for the values of t where the curve crosses y = 0.
	// The quadratic polynomial in t is given by
	//
	//     a t^2 - 2b t + c,
	//
	// where a = p1.y - 2 p2.y + p3.y, b = p1.y - p2.y, and c = p1.y.
	// The discriminant b^2 - ac is clamped to zero, and imaginary
	// roots are treated as a double root at the global minimum
	// where t = b / a.
	vec2 a = p12.xy - p12.zw * 2.0 + p3;
	vec2 b = p12.xy - p12.zw;
	float ra = 1.0 / a.y;
	float rb = 0.5 / b.y;

	float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
	float t1 = (b.y - d) * ra;
	float t2 = (b.y + d) * ra;

	// If the polynomial is nearly linear, then solve -2b t + c = 0.
	if (abs(a.y) < 1.0 / 65536.0)
        t1 = t2 = p12.y * rb;

	// Return the x coordinates where C(t) = 0.
	return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x,
                (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

vec2 solve_vert_poly(vec4 p12, vec2 p3)
{
	// Solve for the values of t where the curve crosses x = 0.
	vec2 a = p12.xy - p12.zw * 2.0 + p3;
	vec2 b = p12.xy - p12.zw;
	float ra = 1.0 / a.x;
	float rb = 0.5 / b.x;

	float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
	float t1 = (b.x - d) * ra;
	float t2 = (b.x + d) * ra;

	// If the polynomial is nearly linear, then solve -2b t + c = 0.
	if (abs(a.x) < 1.0 / 65536.0)
        t1 = t2 = p12.x * rb;

	// Return the y coordinates where C(t) = 0.
	return vec2((a.y * t1 - b.y * 2.0) * t1 + p12.y,
                (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

float calc_coverage(float xcov, float ycov, float xwgt, float ywgt, int flags)
{
	// Combine coverages from the horizontal and vertical rays using their weights.
	// Absolute values ensure that either winding direction convention works.
	float coverage = max(abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0), min(abs(xcov), abs(ycov)));

	// If SLUG_EVENODD is defined during compilation, then check E flag in tex.w. (See vertex shader.)
	#if defined(SLUG_EVENODD)
		if ((flags & 0x1000) == 0)
		{
	#endif
			// Using nonzero fill rule here.
			coverage = clamp(coverage, 0.0, 1.0);
	#if defined(SLUG_EVENODD)
		}
		else
		{
			// Using even-odd fill rule here.
			coverage = 1.0 - abs(1.0 - frac(coverage * 0.5) * 2.0);
		}
	#endif

	// If SLUG_WEIGHT is defined during compilation, then take a square root to boost optical weight.
	#if defined(SLUG_WEIGHT)
		coverage = sqrt(coverage);
	#endif

	return coverage;
}

void main(void)
{
    vec2 ems_per_pixel = fwidth(fs.uv);
	vec2 pixels_per_em = 1.0 / ems_per_pixel;

    vec2 size = fs.max - fs.min;
    vec2 band_size = size / vec2(fs.vband_count, fs.hband_count);
    uint hband = uint(clamp(floor((fs.uv.y - fs.min.y) / band_size.y), 0.0, float(fs.hband_count - 1)));
    uint vband = uint(clamp(floor((fs.uv.x - fs.min.x) / band_size.x), 0.0, float(fs.vband_count - 1)));

    // Horizontal
    float xcov = 0.0;
    float xwgt = 0.0;

    uint h_header_offset = fs.band_offset + hband * 2u;
    uint h_count = pc.band_ptr.data[h_header_offset];
    uint h_offset = pc.band_ptr.data[h_header_offset + 1u];
    for (uint i = 0u; i < h_count; ++i) {
        uint idx = pc.band_ptr.data[fs.band_offset + h_offset + i];
        vec2 p1 = pc.point_ptr.data[idx] - fs.uv;
        vec2 p2 = pc.point_ptr.data[idx + 1] - fs.uv;
        vec4 p12 = vec4(p1, p2);
        vec2 p3 = pc.point_ptr.data[idx + 2] - fs.uv;

        if (max(max(p12.x, p12.z), p3.x) * pixels_per_em.x < -0.5)
            break;

        uint code = calc_root_code(p12.y, p12.w, p3.y);
		if (code != 0u) {
			// At least one root makes a contribution. Calculate them and scale so
			// that the current pixel corresponds to the range [0,1].
			vec2 r = solve_horiz_poly(p12, p3) * pixels_per_em.x;

			// Bits in code tell which roots make a contribution.
			if ((code & 1u) != 0u) {
				xcov += clamp(r.x + 0.5, 0.0, 1.0);
				xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
			}

			if (code > 1u) {
				xcov -= clamp(r.y + 0.5, 0.0, 1.0);
				xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
			}
		}
    }

    // Vertical
    float ycov = 0.0;
    float ywgt = 0.0;

    uint v_header_offset = fs.band_offset + (fs.hband_count + vband) * 2u;
    uint v_count = pc.band_ptr.data[v_header_offset];
    uint v_offset = pc.band_ptr.data[v_header_offset + 1u];
    for (uint i = 0u; i < v_count; ++i) {
        uint idx = pc.band_ptr.data[fs.band_offset + v_offset + i];
        vec2 p1 = pc.point_ptr.data[idx] - fs.uv;
        vec2 p2 = pc.point_ptr.data[idx + 1] - fs.uv;
        vec4 p12 = vec4(p1, p2);
        vec2 p3 = pc.point_ptr.data[idx + 2] - fs.uv;

        if (max(max(p12.y, p12.w), p3.y) * pixels_per_em.y < -0.5)
            break;

		uint code = calc_root_code(p12.x, p12.z, p3.x);
		if (code != 0u) {
			vec2 r = solve_vert_poly(p12, p3) * pixels_per_em.y;
			if ((code & 1u) != 0u) {
				ycov -= clamp(r.x + 0.5, 0.0, 1.0);
				ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
			}
			if (code > 1u) {
				ycov += clamp(r.y + 0.5, 0.0, 1.0);
				ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
			}
		}
    }

    float coverage = calc_coverage(xcov, ycov, xwgt, ywgt, 0);
    frag_color = fs.color * coverage;
}
#endif // FRAGMENT_SHADER
