// Particle volumes (D-039, D-040): a host's dust, smoke or spray, drawn
// after the resolve into each view's HDR target, single-sampled.
//
// Each particle is a sphere of uniform density. It is drawn as a quad facing
// the eye that covers its silhouette exactly (the whole view when the eye is
// inside it or close to it). Each pixel takes the length of its ray inside
// the sphere, cut off where the scene's resolved depth says something nearer
// is (so dust meets the ground and the aircraft softly, without a seam), as
// optical depth: opacity 1 - exp(-extinction x length). Its colour is the
// sun's, the moon's and the sky's light at the eye, scattered toward the eye
// by the particles' albedo and phase function: Cornette and Shanks', "Physically
// reasonable analytic expression for the single-scattering phase function",
// Applied Optics 31(16), 3152-3160 (1992), doi:10.1364/AO.31.003152, as given
// by Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique", EGSR 2020, section 4 (atmosphere.slang's mie_phase; checked
// to integrate to 1 over the sphere). Forward-peaked: dust glows looking
// toward the sun.
//
// Shadowed by the cloud itself, by half-angle slicing (D-041): Green,
// "Volumetric Particle Shadows", NVIDIA (2013), after Kniss et al.'s volume
// shading (GPU Gems, 2004). The particles come sorted along the half vector
// of the view and the light (particle_pack.hpp's half_angle and sort_along)
// and are drawn slice by slice: each slice into the view's particle image,
// lit by the light buffer as the slices nearer the light left it
// (vs_particles, fs_particles), then into the light buffer (vs_light,
// fs_light), which adds each puff's optical depth along the light. The
// particle image is composited over the scene at the end (fs_composite).
// Shadows from the terrain onto the dust aren't drawn yet.

#include "include/atmosphere.slang"
#define FRAME_SET 1
#include "include/frame.slang"

// Mirrors GpuParticle in particle_pack.hpp.
struct Particle {
    float3 position;  // east, north, up from its origin, metres
    float radius;
    float extinction;  // per metre
    uint origin;
    float2 unused;
};
// Mirrors GpuOrigin in particle_pack.hpp.
struct Origin {
    float3 offset;  // from the anchor, ECEF axes, metres
    float unused0;
    float3 east;
    float unused1;
    float3 north;
    float unused2;
    float3 up;
    float unused3;
};
// Mirrors GpuLightSpace in particle_pack.hpp: the light buffer's frame.
struct LightSpace {
    float3 centre;  // the particles' bounding sphere, from the anchor
    float inv_radius;
    float3 u;  // across the light buffer
    float unused0;
    float3 v;
    float erosion;  // the look's (D-051)
    float3 travel;  // the light's direction of travel
    float erosion_gain;  // the eroded density back to the mass (D-051)
};
[[vk::binding(0, 0)]] StructuredBuffer<Particle> particles;
[[vk::binding(1, 0)]] StructuredBuffer<Origin> origins;
[[vk::binding(2, 0)]] StructuredBuffer<LightSpace> light_space;
// Each view's drawing order (its sort along the half vector), from
// push.order_base.
[[vk::binding(3, 0)]] StructuredBuffer<uint> order;
// The view's resolved depth (reverse-Z, infinite far plane), read by pixel.
[[vk::binding(0, 2)]] Texture2D<float> scene_depth;
// The light buffer: optical depth toward the light, by light-space position.
[[vk::binding(1, 2)]] Texture2D<float> light_depth;
[[vk::binding(2, 2)]] SamplerState light_sampler;
// The view's particle image (premultiplied), for the composite.
[[vk::binding(3, 2)]] Texture2D<float4> particle_image;
// The detail's noise (D-043, particle_noise.hpp): inverted cellular noise,
// and curl noise, each tiling, sampled linearly and wrapping.
[[vk::binding(4, 2)]] Texture3D<float> detail_noise;
[[vk::binding(5, 2)]] Texture3D<float4> curl_noise;
[[vk::binding(6, 2)]] SamplerState noise_sampler;

// Mirrors ParticlePush in particles.cpp.
struct ParticlePush {
    float4x4 ecef_to_clip;  // the view's, for points relative to the eye
    float3 anchor;          // the origins' anchor relative to the eye
    float forward_scattering;
    float3 albedo;
    uint order_base;  // this view's first entry in `order`
    uint depth_scale;  // scene depth pixels per particle image pixel: 1, or 2 at half resolution
    float unused;
    float2 inv_target;  // 1 / the view's render size, pixels (the composite)
    float time;         // seconds the detail has drifted (D-043)
    float detail;       // 0 to 1: how much of the density the noise erodes
    float detail_size;  // the noise's features, metres
    float swirl;        // the curl's reach, in features
};
[[vk::push_constant]] ConstantBuffer<ParticlePush> push;

struct ParticleVertex {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 ray;  // the quad's point relative to the eye, ECEF axes
    [[vk::location(1)]] nointerpolation float3 centre;
    [[vk::location(2)]] nointerpolation float radius;
    [[vk::location(3)]] nointerpolation float extinction;
    [[vk::location(4)]] nointerpolation float3 enu;  // its centre, east, north, up from its origin
    [[vk::location(5)]] nointerpolation uint origin;
};

// The near plane's distance: with an infinite reverse-Z projection, clip z
// is the near distance for every point.
float near_distance() {
    return push.ecef_to_clip[2].w;
}

static const float2 kCorners[6] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
                                    float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0) };

// A particle's centre relative to the anchor.
float3 centre_from_anchor(Particle p) {
    const Origin o = origins[p.origin];
    return o.offset + o.east * p.position.x + o.north * p.position.y + o.up * p.position.z;
}

// A point (from the anchor) in the light buffer: 0 to 1 across it.
float2 light_uv(float3 from_anchor) {
    const LightSpace l = light_space[0];
    const float3 d = (from_anchor - l.centre) * l.inv_radius;
    return float2(dot(d, l.u), dot(d, l.v)) * 0.5 + 0.5;
}

[shader("vertex")]
ParticleVertex vs_particles(uint id: SV_VulkanVertexID) {
    const Particle p = particles[order[push.order_base + id / 6]];
    const float2 corner = kCorners[id % 6];
    const float3 c = push.anchor + centre_from_anchor(p);
    // The view's axes in ECEF, from its matrix's rows (projection.cpp):
    // forward is clip w's; clip x's and y's carry a forward part in an
    // asymmetric frustum, taken out so the axes are square.
    const float3 forward = normalize(push.ecef_to_clip[3].xyz);
    const float3 rx = push.ecef_to_clip[0].xyz;
    const float3 ry = push.ecef_to_clip[1].xyz;
    const float3 right = normalize(rx - dot(rx, forward) * forward);
    const float3 up = normalize(ry - dot(ry, forward) * forward);
    const float near = near_distance();
    const float d = length(c);
    float3 at;
    if (d <= p.radius * 1.02 + 4.0 * near) {
        // The eye inside it, or nearly: a quad across the whole view, past
        // the near plane, out to 89.94 degrees from the view's axis (tan
        // 1000): wider than any channel (the config allows edges below 90).
        // The rasterizer clips what's outside the view.
        const float D = 8.0 * near;
        at = forward * D + (right * corner.x + up * corner.y) * (1000.0 * D);
    } else {
        // Facing the eye, through the centre, as wide as the cone of rays
        // that touch the sphere is there.
        const float3 dir = c / d;
        float3 u = cross(dir, up);
        u = dot(u, u) > 1e-6 ? normalize(u) : right;
        const float3 v = cross(u, dir);
        const float half = p.radius * d / sqrt(d * d - p.radius * p.radius);
        at = c + (u * corner.x + v * corner.y) * half;
    }
    ParticleVertex o2;
    o2.position = mul(push.ecef_to_clip, float4(at, 1.0));
    o2.ray = at;
    o2.centre = c;
    o2.radius = p.radius;
    o2.extinction = p.extinction;
    o2.enu = p.position;
    o2.origin = p.origin;
    if (p.extinction <= 0.0) {
        o2.position = float4(2.0, 2.0, 0.0, 1.0);  // outside the view
    }
    return o2;
}

// Light scattered more than once inside a thick cloud is what makes it
// bright, as clouds are white though a droplet scatters mostly forward.
// Adapted from Wrenninge, Kulla and Lundqvist, "Oz: The Great and
// Volumetric", ACM SIGGRAPH 2013 Talks, 46:1, doi:10.1145/2504459.2504518,
// equations 1 and 2: L = sum over octaves i < N of
//   sigma_s b^i L_light p(w_i, w_o, c^i g) exp(-a^i * the light path's optical depth)
// with their defaults a = b = c = 1/2 (and N = 8). Hillaire used it for
// Frostbite's clouds ("Physically Based Sky, Atmosphere and Cloud Rendering
// in Frostbite", SIGGRAPH 2016 course notes, section 5, page 41). Here:
//   - b = c = 1/2, as their defaults: each octave half the light and half
//     the phase function's eccentricity of the one before;
//   - N = 3, not 8: the octaves are evaluated for every pixel of every
//     overlapping puff, and the five left out are 12 % of the eight's sum
//     (0.24 of 1.99);
//   - a = 1/2, their default: each octave's light reaches the point
//     through a^i of the light path's optical depth, from the light buffer
//     (D-041). With a = b the scattering is scaled no more than the
//     extinction, the condition Hillaire gives for energy conservation
//     (page 41, equation 20, his letters swapped: his a <= b is b <= a in
//     Wrenninge's). The octaves still sum to 1.75 times the light once
//     scattered where nothing shadows, as the approximation does; the look
//     is tuned by [particles]' albedo (D-040).
// `depth` is the optical depth toward the light.
float multiple_scattering_phase(float g, float c, float depth) {
    float sum = 0.0;
    float energy = 1.0;
    float through = 1.0;  // a^i
    for (int k = 0; k < 3; ++k) {
        sum += energy * mie_phase(g, c) * exp(-through * depth);
        energy *= 0.5;
        g *= 0.5;
        through *= 0.5;
    }
    return sum;
}
static const float kOctaveEnergy = 1.75;  // b^0 + b^1 + b^2: the octaves' sum for an isotropic light

// Detail in the particles (D-043): the density at a point as a fraction of
// a particle's uniform density, `enu` the point in the particle's origin's
// frame (metres), `r2` its squared distance from the particle's centre in
// radii. After Schneider and Vos ("The Real-time Volumetric Cloudscapes of
// Horizon: Zero Dawn", SIGGRAPH 2015 Advances in Real-Time Rendering; the
// slides read), whose detail is inverted cellular noise at increasing
// frequencies ("tightly packed billow shapes", slides 31 and 34), applied
// to the cloud's base shape and "distort[ed] ... by our 2d curl noise to
// fake the swirly distortions from atmospheric turbulence" (slide 39). As
// theirs, the noise is one field in the world (here the origin's frame),
// not one per particle: overlapping particles make one cloud, and its
// billows run across them. Theirs erodes only the edges of a cloud they
// draw from one density field; here a cloud is many particles, its inside
// the inside of each, so the field varies the density everywhere: the
// fraction is base x (1 - detail (1 - n)) / (1 - detail kDetailMean'),
// the division keeping the particles' mass on average (the sender's
// density is a mass; the detail moves it, doesn't remove it). The base is
// the sphere softened over its outer part, min(1, 3 (1 - r^2)): 1 inside
// r^2 = 2/3, 0 at the surface, so a particle's own edge doesn't show as a
// sphere's; divided by its mean over the sphere's volume, the integral of
// 3 r^2 min(1, 3 (1 - r^2)) from 0 to 1, 0.7645, so it keeps the mass too
// (the outer shell holds 46 % of the volume). Their
// curl texture is 2D; here it's a 3D volume, the curl of a vector
// potential (Bridson, Hourihan and Nordenstam, "Curl-Noise for Procedural
// Fluid Flow", SIGGRAPH 2007, equation 1; read), drifting in time: billows
// that swirl without gathering anywhere, as the curl has no sinks. Both
// volumes are 32^3 with 4 features a side at their first octave, so a tile
// spans 4 features of detail_size metres.
static const float kDetailMean = 0.4586;  // the detail volume's mean (particle_noise.hpp; tested)
static const float kBaseMean = 0.7645;    // the base's mean over the sphere
// The noise's factor at a point (metres in the origin's frame): 1 on
// average over the field.
// The field at a point (metres in the origin's frame), 0 to 1, swirled.
float detail_at(float3 enu) {
    float3 at = enu / (4.0 * push.detail_size);
    const float3 curl_at = at * 0.5 + float3(0.0, 0.0, 0.03 * push.time);
    at += curl_noise.SampleLevel(noise_sampler, curl_at, 0.0).xyz * (0.25 * push.swirl);
    return detail_noise.SampleLevel(noise_sampler, at, 0.0);
}
// The noise's factor for the field's value there: 1 on average over the field.
float detail_factor(float n) {
    return (1.0 - push.detail * (1.0 - n)) / (1.0 - push.detail * (1.0 - kDetailMean));
}
// The particle eaten by the field (D-051), after Schneider and Vos's
// clouds, whose detail is subtracted "at the edges of the cloud" (SIGGRAPH
// 2015, slide 39; read for D-043). Against s = 1 - r^2 (1 at the centre, 0
// at the surface), less t = erosion x (1 - n), stretched back to 1 at the
// centre: the particle ends where s = t, anywhere from its surface (the
// field high) in to sqrt(erosion) of its radius from it (the field low), so
// its outline follows the field's billows, not a sphere. Eroding only the
// base's outer shell (r^2 past 2/3) was tried first: a small, dense puff is
// mostly core, and stayed a ball. The base w is kept inside, so with no
// erosion it's as before: w at erosion 0.
float eroded(float w, float r2, float n, float erosion) {
    if (erosion <= 0.0) {
        return w;
    }
    const float t = erosion * (1.0 - n);
    const float s = max(1.0 - r2, 0.0);
    return w * saturate((s - t) / max(1.0 - t, 1e-3) * 4.0);
}
// The base, min(1, 3 (1 - r^2)), integrated along a line `b2` (squared, in
// radii) from the centre, from its closest point to `s` radii along it
// (odd in s). Exact: 1 while r^2 < 2/3, then 3 (1 - b2 - s^2), to the
// surface; the samples would miss the soft ends of every chord (on the
// one through the centre, 4 midpoints all fall where the base is 1: 10 %
// too much).
float base_along(float s, float b2) {
    const float a = sqrt(max(2.0 / 3.0 - b2, 0.0));  // where r^2 reaches 2/3
    const float e = sqrt(max(1.0 - b2, 0.0));        // the surface
    const float x = min(abs(s), e);
    float g = min(x, a);
    if (x > a) {
        g += 3.0 * ((1.0 - b2) * (x - a) - (x * x * x - a * a * a) / 3.0);
    }
    return s < 0.0 ? -g : g;
}
static const int kDetailSamples = 4;  // along each pixel's chord through a puff
// With erosion (D-051): its edges are sharper than the detail's variation, and
// 4 samples took 14 % of a thin puff's cover (8: 5 %).
static const int kErodedSamples = 8;

[shader("fragment")]
float4 fs_particles(ParticleVertex i) : SV_Target {
    const float3 dir = normalize(i.ray);
    // Where the ray enters and leaves the sphere, in floats: Haines, Guenther
    // and Akenine-Moeller, "Precision Improvements for Ray/Sphere
    // Intersection", Ray Tracing Gems (2019), chapter 7. The discriminant as
    // r^2 - l^2 (their equation 5, after Hearn and Baker), l the centre's
    // distance from the ray, not b^2 - (|c|^2 - r^2), which loses a small
    // puff far off; the roots as q and c/q (their equation 10, after Press et
    // al.), not b -/+ sqrt: about twice as precise for the nearer one when
    // the eye is close to a large puff's surface (c = |c|^2 - r^2 still
    // cancels there, as theirs does), well under a millimetre either way.
    // Here the centre is relative to the eye and the ray's direction is a
    // unit vector, so their b' is b and a is 1.
    const float b = dot(dir, i.centre);
    const float3 l = i.centre - b * dir;
    const float disc = i.radius * i.radius - dot(l, l);
    if (disc <= 0.0) {
        discard;
    }
    const float q = b + (b >= 0.0 ? sqrt(disc) : -sqrt(disc));  // not 0: disc > 0
    const float root_q = q;
    const float root_cq = (dot(i.centre, i.centre) - i.radius * i.radius) / q;
    const float enter = max(min(root_q, root_cq), 0.0);
    float leave = max(root_q, root_cq);
    // Cut off where the scene is nearer: depth 0 is the sky. At half
    // resolution, the nearest (reverse-Z: greatest) of the scene pixels
    // this one covers, so dust doesn't show over what's in front: a half
    // image of an odd size, (n + 1) / 2, covers a little under 2 a pixel,
    // so the range is worked out from the sizes, up to 3 a side.
    uint depth_w;
    uint depth_h;
    scene_depth.GetDimensions(depth_w, depth_h);
    const int2 full = int2(int(depth_w), int(depth_h));
    const int2 here = int2(i.position.xy);
    int2 lo = here;
    int2 hi = here;
    if (push.depth_scale > 1) {
        const int2 half_size = (full + 1) / 2;
        lo = (here * full) / half_size;                       // floor(here * full / half)
        hi = ((here + 1) * full + half_size - 1) / half_size - 1;  // ceil(...) - 1
    }
    lo = clamp(lo, int2(0, 0), full - 1);
    hi = clamp(hi, lo, full - 1);
    float depth = 0.0;
    for (int y = lo.y; y <= hi.y; ++y) {
        for (int x = lo.x; x <= hi.x; ++x) {
            depth = max(depth, scene_depth.Load(int3(x, y, 0)));
        }
    }
    if (depth > 0.0) {
        const float view_z = near_distance() / depth;
        const float along = dot(push.ecef_to_clip[3].xyz, dir);  // view z per metre of ray
        leave = min(leave, view_z / max(along, 1e-6));
    }
    const float length_inside = leave - enter;
    if (length_inside <= 0.0) {
        discard;
    }
    // The chord's optical depth: uniform, or with detail the mean of the
    // eroded density at kDetailSamples points along it (D-043).
    float density = 1.0;
    if (push.detail > 0.0) {
        // The samples' offsets from the centre turned into the origin's
        // east, north and up (ECEF axes to ENU).
        // The chord's base, exactly, over its length (the line's closest
        // point to the centre is at b along the ray), then the noise's
        // factor at kDetailSamples points along it, weighted by the base
        // there.
        const Origin o = origins[i.origin];
        const float inv_r = 1.0 / i.radius;
        const float b2 = dot(l, l) * inv_r * inv_r;  // l: the centre's offset from the ray
        const float base = (base_along((leave - b) * inv_r, b2) - base_along((enter - b) * inv_r, b2)) /
                           (length_inside * inv_r * kBaseMean);
        float weighted = 0.0;
        float weights = 0.0;
        const int samples = light_space[0].erosion > 0.0 ? kErodedSamples : kDetailSamples;
        const float step = length_inside / float(samples);
        for (int k = 0; k < samples; ++k) {
            const float t = enter + (float(k) + 0.5) * step;
            const float3 off = dir * t - i.centre;
            const float w = saturate((1.0 - dot(off, off) * inv_r * inv_r) * 3.0);
            const float3 enu = i.enu + float3(dot(off, o.east), dot(off, o.north), dot(off, o.up));
            const float n = detail_at(enu);
            // Each sample weighted by the base there, eroded (D-051), over the
            // base's own weights: with no erosion, as before.
            weighted += eroded(w, dot(off, off) * inv_r * inv_r, n, light_space[0].erosion) * detail_factor(n);
            weights += w;
        }
        // The erosion's mass given back (D-051): 1 without erosion.
        density = base * (weights > 0.0 ? weighted / weights : 1.0) * light_space[0].erosion_gain;
    }
    const float opacity = 1.0 - exp(-i.extinction * density * length_inside);
    if (opacity < 1e-4) {
        discard;
    }
    // The light it scatters toward the eye: the sun's through the cloud
    // between it and the middle of this ray's path in the puff (the light
    // buffer, as the slices nearer the light left it), and the moon's,
    // unshadowed (the light buffer is the sun's), by the phase function; the
    // sky's as if from the whole upper hemisphere; each scattered several
    // times (multiple_scattering_phase).
    const float3 middle = dir * (0.5 * (enter + leave)) - push.anchor;  // from the anchor
    const float sun_depth = max(light_depth.SampleLevel(light_sampler, light_uv(middle), 0.0), 0.0);
    const ViewSky sky = view_sky[lighting.view];
    const float g = push.forward_scattering;
    const float3 light =
        sky.sun_irradiance * multiple_scattering_phase(g, dot(dir, lighting.sun), sun_depth) +
        sky.moon_irradiance * multiple_scattering_phase(g, dot(dir, lighting.moon), 0.0) +
        sky.sky_irradiance * (0.5 * kInvPi * kOctaveEnergy);
    const float3 radiance = push.albedo * light * sky.exposure;
    return float4(min(radiance, float3(60000.0)) * opacity, opacity);
}

// The light pass: each puff drawn as the light sees it, orthographic along
// its travel, into the light buffer (R16F, added: optical depths sum, in
// any order within a slice). The quad is the sphere's silhouette, exactly.
struct LightVertex {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 across;  // from the centre, in radii
    [[vk::location(1)]] nointerpolation float radius;
    [[vk::location(2)]] nointerpolation float extinction;
};

[shader("vertex")]
LightVertex vs_light(uint id: SV_VulkanVertexID) {
    const Particle p = particles[order[push.order_base + id / 6]];
    const float2 corner = kCorners[id % 6];
    const LightSpace l = light_space[0];
    const float3 d = (centre_from_anchor(p) - l.centre) * l.inv_radius;
    const float2 at = float2(dot(d, l.u), dot(d, l.v)) + corner * (p.radius * l.inv_radius);
    LightVertex o;
    o.position = float4(at, 0.5, 1.0);  // light_uv's mapping: uv = ndc / 2 + 1/2
    o.across = corner;
    o.radius = p.radius;
    o.extinction = p.extinction;
    if (p.extinction <= 0.0) {
        o.position = float4(2.0, 2.0, 0.5, 1.0);
    }
    return o;
}

[shader("fragment")]
float4 fs_light(LightVertex i) : SV_Target {
    // A ray along the light at distance |across| radii from the centre
    // crosses 2 sqrt(1 - |across|^2) radii of the sphere.
    const float inside = 1.0 - dot(i.across, i.across);
    if (inside <= 0.0) {
        discard;
    }
    return float4(i.extinction * 2.0 * i.radius * sqrt(inside), 0.0, 0.0, 0.0);
}

// The composite: the view's particle image (premultiplied colour, and its
// coverage in alpha) over the scene, one triangle covering the view.
struct CompositeVertex {
    float4 position : SV_Position;
};

[shader("vertex")]
CompositeVertex vs_composite(uint id: SV_VulkanVertexID) {
    CompositeVertex o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_composite(CompositeVertex i) : SV_Target {
    // Bilinear: at full resolution each pixel its own (sampled at its
    // centre); at half, filtered up (D-042).
    return particle_image.SampleLevel(light_sampler, i.position.xy * push.inv_target, 0.0);
}
