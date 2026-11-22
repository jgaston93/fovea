// Particle volumes (D-039, D-040): a host's dust, smoke or spray, drawn
// after the resolve into each view's HDR target, single-sampled.
//
// Each particle is an anisotropic Gaussian (D-053), out to 3 sigma. It is
// drawn as a quad facing the eye that covers a sphere holding it (the whole
// view when the eye is inside it or close to it). Each pixel takes the
// Gaussian's optical depth along its ray, exactly (by erf, in the particle's
// whitened space, where it's round), cut off where the scene's resolved
// depth says something nearer is (so dust meets the ground and the aircraft
// softly, without a seam): opacity 1 - exp(-optical depth). Its colour is the
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

// Mirrors GpuParticle in particle_pack.hpp: an anisotropic Gaussian (D-053),
// its extinction coefficient at an offset x (east, north, up) from its centre
// peak exp(-|W x|^2 / 2), drawn out to |W x| = 3 inside `radius`.
struct Particle {
    float3 position;  // east, north, up from its origin, metres
    float radius;     // a sphere holding its 3-sigma ellipsoid, metres
    float extinction; // its extinction cross-section x its opacity, m^2
    float peak;       // its extinction coefficient at the centre, per metre
    uint origin;
    float unused;
    float4 whiten_a;  // W, lower triangular: w00, w10, w11, w20
    float2 whiten_b;  // w21, w22
    float2 pad;
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
// The volume's box and sizes (D-052), when [particles] renderer = "volume":
// mirrors GpuVolumeSpace in particle_pack.hpp.
struct VolumeSpace {
    float3 corner;    // the box's lowest corner from the anchor, along `axes` (ECEF)
    float voxel;      // a voxel's side, metres
    uint size;        // voxels a side of the density volume
    uint light_size;  // voxels a side of the sun's volume
    float inv_voxel;
    uint particles;   // how many to inject
    float unused0;
    float inv_reference;  // 1 / the particles' mean peak extinction coefficient [m]: the erosion's scale
    float2 unused;
};
[[vk::binding(4, 0)]] StructuredBuffer<VolumeSpace> volume_space;
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
// The volume (D-052): the density (extinction per metre) and the optical
// depth toward the sun, both over the box, sampled filtered.
[[vk::binding(8, 2)]] Texture3D<float> volume_density;
[[vk::binding(9, 2)]] Texture3D<float> volume_sun_depth;
// Written by the volume's compute passes (set 3).
[[vk::binding(0, 3)]] RWTexture3D<uint> volume_accumulate;
[[vk::binding(1, 3)]] RWTexture3D<float> volume_density_out;
[[vk::binding(2, 3)]] RWTexture3D<float> volume_sun_out;

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
    [[vk::location(2)]] nointerpolation float3 enu;  // its centre, east, north, up from its origin
    [[vk::location(3)]] nointerpolation uint origin;
    [[vk::location(4)]] nointerpolation uint index;  // into the particles (its whitening, D-053)
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

// An ECEF vector in an origin's east, north and up.
float3 to_enu(Origin o, float3 v) {
    return float3(dot(v, o.east), dot(v, o.north), dot(v, o.up));
}
// An offset (east, north, up) from a particle's centre in its whitened space,
// where it's a round unit Gaussian (D-053).
float3 whiten(Particle p, float3 enu) {
    return float3(p.whiten_a.x * enu.x, p.whiten_a.y * enu.x + p.whiten_a.z * enu.y,
                  p.whiten_a.w * enu.x + p.whiten_b.x * enu.y + p.whiten_b.y * enu.z);
}
// Abramowitz and Stegun's 7.1.26 (particle_pack.cpp's erf_approx, its error
// tested against std::erf).
float erf_approx(float x) {
    const float a = abs(x);
    const float t = 1.0 / (1.0 + 0.3275911 * a);
    const float poly = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 +
                       t * (-1.453152027 + t * 1.061405429))));
    const float y = 1.0 - poly * exp(-a * a);
    return x < 0.0 ? -y : y;
}
// The integral of exp(-|t dw - cw|^2 / 2) over t from t0 to t1: a whitened
// ray (direction dw, the centre cw from its start) through a unit Gaussian.
// Exact: a Gaussian in t about its closest point m, by erf.
float gauss_along(float3 dw, float3 cw, float t0, float t1) {
    const float a = max(dot(dw, dw), 1e-12);
    const float m = dot(dw, cw) / a;
    const float k = max(dot(cw, cw) - m * m * a, 0.0);
    const float s = sqrt(0.5 * a);
    return exp(-0.5 * k) * sqrt(kPi / (2.0 * a)) * (erf_approx(s * (t1 - m)) - erf_approx(s * (t0 - m)));
}

// A point (from the anchor) in the light buffer: 0 to 1 across it.
float2 light_uv(float3 from_anchor) {
    const LightSpace l = light_space[0];
    const float3 d = (from_anchor - l.centre) * l.inv_radius;
    return float2(dot(d, l.u), dot(d, l.v)) * 0.5 + 0.5;
}

[shader("vertex")]
ParticleVertex vs_particles(uint id: SV_VulkanVertexID) {
    const uint index = order[push.order_base + id / 6];
    const Particle p = particles[index];
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
    o2.enu = p.position;
    o2.origin = p.origin;
    o2.index = index;
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
// For a Gaussian (D-053), s is its density relative to its centre's, w
// itself: eroded where it's thin.
float eroded(float w, float s, float n, float erosion) {
    if (erosion <= 0.0) {
        return w;
    }
    const float t = erosion * (1.0 - n);
    return w * saturate((s - t) / max(1.0 - t, 1e-3) * 4.0);
}
static const int kDetailSamples = 4;  // along each pixel's chord through a puff
// With erosion (D-051): its edges are sharper than the detail's variation, and
// 4 samples took 14 % of a thin puff's cover (8: 5 %).
static const int kErodedSamples = 8;

[shader("fragment")]
float4 fs_particles(ParticleVertex i) : SV_Target {
    const float3 dir = normalize(i.ray);
    // The ray in the particle's whitened space (D-053), where it's a round
    // unit Gaussian: y(t) = t dw - cw, t metres along the ray from the eye.
    const Particle p = particles[i.index];
    const Origin o = origins[i.origin];
    const float3 dw = whiten(p, to_enu(o, dir));
    const float3 cw = whiten(p, to_enu(o, i.centre));
    // Where it enters and leaves the 3-sigma ellipsoid, |y| = 3: the roots of
    // a t^2 - 2 b t + c = 0, the discriminant as a (9 - l^2), l the centre's
    // distance from the whitened ray, and the roots as q / a and c / q, as
    // the sphere's were (Haines, Guenther and Akenine-Moeller, "Precision
    // Improvements for Ray/Sphere Intersection", Ray Tracing Gems, 2019,
    // chapter 7, equations 5 and 10).
    const float a = max(dot(dw, dw), 1e-12);
    const float b = dot(dw, cw);
    const float3 l = cw - (b / a) * dw;
    const float disc = a * (9.0 - dot(l, l));
    if (disc <= 0.0) {
        discard;
    }
    const float q = b + (b >= 0.0 ? sqrt(disc) : -sqrt(disc));  // not 0: disc > 0
    const float root_q = q / a;
    const float root_cq = (dot(cw, cw) - 9.0) / q;
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
    // The Gaussian's optical depth along the ray, exactly (gauss_along); with
    // detail (D-043), its factor at samples along it, weighted by the
    // Gaussian there, and eroded (D-051) where the Gaussian is thin.
    float tau = p.peak * gauss_along(dw, cw, enter, leave);
    if (push.detail > 0.0) {
        float weighted = 0.0;
        float weights = 0.0;
        const int samples = light_space[0].erosion > 0.0 ? kErodedSamples : kDetailSamples;
        const float step = length_inside / float(samples);
        for (int k = 0; k < samples; ++k) {
            const float t = enter + (float(k) + 0.5) * step;
            const float3 off = to_enu(o, dir * t - i.centre);
            const float3 y = whiten(p, off);
            const float w = exp(-0.5 * dot(y, y));
            const float n = detail_at(i.enu + off);
            weighted += eroded(w, w, n, light_space[0].erosion) * detail_factor(n);
            weights += w;
        }
        // The erosion's mass given back (D-051): 1 without erosion.
        tau *= (weights > 0.0 ? weighted / weights : 1.0) * light_space[0].erosion_gain;
    }
    const float opacity = 1.0 - exp(-tau);
    if (opacity < 1e-4) {
        discard;
    }
    // The light it scatters toward the eye: the sun's through the cloud
    // between it and the middle of this ray's path in the puff (the light
    // buffer, as the slices nearer the light left it), and the moon's,
    // unshadowed (the light buffer is the sun's), by the phase function; the
    // sky's as if from the whole upper hemisphere; each scattered several
    // times (multiple_scattering_phase).
    // At the Gaussian's densest along the ray, within what's drawn.
    const float3 middle = dir * clamp(b / a, enter, leave) - push.anchor;  // from the anchor
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
    [[vk::location(0)]] float2 across;  // from the centre, in bounds' radii
    // Across, the light-space axes u and v times the bound, and the light's
    // travel, each in the particle's whitened space (D-053).
    [[vk::location(1)]] nointerpolation float3 du;
    [[vk::location(2)]] nointerpolation float3 dv;
    [[vk::location(3)]] nointerpolation float3 dt;
    [[vk::location(4)]] nointerpolation float peak;
};

[shader("vertex")]
LightVertex vs_light(uint id: SV_VulkanVertexID) {
    const Particle p = particles[order[push.order_base + id / 6]];
    const float2 corner = kCorners[id % 6];
    const LightSpace l = light_space[0];
    const Origin org = origins[p.origin];
    const float3 d = (centre_from_anchor(p) - l.centre) * l.inv_radius;
    const float2 at = float2(dot(d, l.u), dot(d, l.v)) + corner * (p.radius * l.inv_radius);
    LightVertex o;
    o.position = float4(at, 0.5, 1.0);  // light_uv's mapping: uv = ndc / 2 + 1/2
    o.across = corner;
    o.du = whiten(p, to_enu(org, l.u * p.radius));
    o.dv = whiten(p, to_enu(org, l.v * p.radius));
    o.dt = whiten(p, to_enu(org, l.travel));
    o.peak = p.peak;
    if (p.extinction <= 0.0) {
        o.position = float4(2.0, 2.0, 0.5, 1.0);
    }
    return o;
}

[shader("fragment")]
float4 fs_light(LightVertex i) : SV_Target {
    // The line along the light through this point, whitened: y(s) = c0 + s dt.
    // Its optical depth through the whole Gaussian, peak sqrt(2 pi / a)
    // exp(-k / 2), k its closest approach squared; none if it misses the
    // 3-sigma ellipsoid.
    const float3 c0 = i.across.x * i.du + i.across.y * i.dv;
    const float a = max(dot(i.dt, i.dt), 1e-12);
    const float b = dot(i.dt, c0);
    const float k = dot(c0, c0) - b * b / a;
    if (k >= 9.0) {
        discard;
    }
    return float4(i.peak * sqrt(2.0 * kPi / a) * exp(-0.5 * k), 0.0, 0.0, 0.0);
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

// ---- The volume (D-052) ----
//
// One box over the particles, in the anchor's frame with ECEF axes, shared
// by every view: each particle's extinction added into it (fixed point,
// atomics: Hillaire 2015's particle voxelization, its "high quality"
// sphere fill), converted to floats, then the optical depth toward the sun
// marched over it once a frame (his volumetric shadow maps), and each view
// marched through it per pixel.

// Fixed point for the atomic adds: an extinction of 1 per metre is 2048
// (Hillaire 2015, slide 54's scale).
static const float kVolumeScale = 65536.0;  // 1/65536 per metre; a voxel holds up to 65536 per metre

// A point (from the anchor) in the box: 0 to 1 along each axis.
float3 volume_uvw(float3 from_anchor) {
    const VolumeSpace v = volume_space[0];
    return (from_anchor - v.corner) * (v.inv_voxel / float(v.size));
}

// A particle's extinction into the voxels it overlaps (D-053): its Gaussian
// at each voxel's centre, out to 3 sigma, scaled so the voxels hold its
// whole extinction cross-section exactly (summed at the centres alone, a
// particle a voxel or two across kept anywhere from 0.6 to 1.8 of it, and
// moving ones flickered: D-052). One whose bound is under a voxel goes into
// the eight around its centre by their trilinear weights.
groupshared float inject_weights[64];

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_volume_inject(uint3 group: SV_GroupID, uint3 thread: SV_GroupThreadID) {
    const VolumeSpace v = volume_space[0];
    const uint index = group.x + group.y * 65535u;
    if (index >= v.particles) {
        return;
    }
    const Particle p = particles[index];
    if (p.extinction <= 0.0) {
        return;
    }
    const Origin o = origins[p.origin];
    const float3 c = (centre_from_anchor(p) - v.corner) * v.inv_voxel;  // in voxels
    const float r = p.radius * v.inv_voxel;
    const int n = int(v.size);
    // Its extinction cross-section per voxel's volume: what the voxels'
    // extinction coefficients (per metre) sum to.
    const float total = p.extinction * (v.inv_voxel * v.inv_voxel * v.inv_voxel);
    if (r < 1.0) {
        if (thread.x != 0) {
            return;
        }
        const float3 g = c - 0.5;
        const int3 lo = int3(floor(g));
        const float3 f = g - float3(lo);
        for (int k = 0; k < 8; ++k) {
            const int3 b = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
            const int3 at = lo + b;
            if (any(at < 0) || any(at >= n)) {
                continue;
            }
            const float3 w3 = select(b == 1, f, 1.0 - f);
            InterlockedAdd(volume_accumulate[uint3(at)],
                           uint(w3.x * w3.y * w3.z * total * kVolumeScale + 0.5));
        }
        return;
    }
    // The weights summed first over the whole 3-sigma bound (the box's edge
    // aside), then the particle scaled to its cross-section exactly. The
    // early return above is uniform per group.
    const int3 lo_all = int3(floor(c - r));
    const int3 span_all = int3(ceil(c + r)) - lo_all;
    const uint count_all = uint(span_all.x * span_all.y * span_all.z);
    float sum = 0.0;
    for (uint k = thread.x; k < count_all; k += 64u) {
        const int3 at = lo_all + int3(int(k % uint(span_all.x)),
                                      int((k / uint(span_all.x)) % uint(span_all.y)),
                                      int(k / uint(span_all.x * span_all.y)));
        const float3 y = whiten(p, to_enu(o, (float3(at) + 0.5 - c) * v.voxel));
        const float q = dot(y, y);
        sum += q < 9.0 ? exp(-0.5 * q) : 0.0;
    }
    inject_weights[thread.x] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (thread.x < stride) {
            inject_weights[thread.x] += inject_weights[thread.x + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    const float per_weight = total / max(inject_weights[0], 1e-6);
    const int3 lo = max(lo_all, int3(0, 0, 0));
    const int3 hi = min(lo_all + span_all, int3(n, n, n));
    const int3 span = max(hi - lo, int3(0, 0, 0));
    const uint count = uint(span.x * span.y * span.z);
    for (uint k = thread.x; k < count; k += 64u) {
        const int3 at = lo + int3(int(k % uint(span.x)), int((k / uint(span.x)) % uint(span.y)),
                                  int(k / uint(span.x * span.y)));
        const float3 y = whiten(p, to_enu(o, (float3(at) + 0.5 - c) * v.voxel));
        const float q = dot(y, y);
        if (q >= 9.0) {
            continue;
        }
        InterlockedAdd(volume_accumulate[uint3(at)],
                       uint(exp(-0.5 * q) * per_weight * kVolumeScale + 0.5));
    }
}

// The fixed point to extinction per metre.
[shader("compute")]
[numthreads(4, 4, 4)]
void cs_volume_convert(uint3 id: SV_DispatchThreadID) {
    const uint n = volume_space[0].size;
    if (any(id >= n)) {
        return;
    }
    volume_density_out[id] = float(volume_accumulate[id]) / kVolumeScale;
}

// The optical depth from each voxel of the sun's volume toward the sun, to
// the box's edge, marched a voxel of the density at a time (nearest: the
// density volume is being read as an image here).
[shader("compute")]
[numthreads(4, 4, 4)]
void cs_volume_sun(uint3 id: SV_DispatchThreadID) {
    const VolumeSpace v = volume_space[0];
    if (any(id >= v.light_size)) {
        return;
    }
    const float3 toward_sun = -light_space[0].travel;
    const float side = float(v.size) * v.voxel;
    float3 at = v.corner + (float3(id) + 0.5) * (side / float(v.light_size));
    float depth = 0.0;
    const int n = int(v.size);
    for (int k = 0; k < 4 * n; ++k) {
        at += toward_sun * v.voxel;
        const int3 cell = int3(floor((at - v.corner) * v.inv_voxel));
        if (any(cell < 0) || any(cell >= n)) {
            break;
        }
        depth += volume_density_out[uint3(cell)] * v.voxel;
    }
    volume_sun_out[id] = depth;
}

struct VolumeVertex {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 ndc;
};

[shader("vertex")]
VolumeVertex vs_volume(uint id: SV_VulkanVertexID) {
    VolumeVertex o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.ndc = uv * 2.0 - 1.0;
    o.position = float4(o.ndc, 0.0, 1.0);
    return o;
}

// Where a ray enters and leaves the box (the eye at the origin, the box's
// corner relative to the eye): t in metres along the unit direction.
float2 box_span(float3 corner, float side, float3 dir) {
    // No component exactly 0 (0 * inf is NaN where a corner's is 0 too).
    const float3 safe = select(abs(dir) < 1e-12, float3(1e-12, 1e-12, 1e-12), dir);
    const float3 inv = 1.0 / safe;
    const float3 a = (corner) * inv;
    const float3 b = (corner + side) * inv;
    const float3 lo = min(a, b);
    const float3 hi = max(a, b);
    return float2(max(max(lo.x, lo.y), max(lo.z, 0.0)), min(min(hi.x, hi.y), hi.z));
}

// A pixel's march through the volume: the light it scatters toward the eye
// over the dust before the scene, premultiplied, and its cover; as the
// puffs' particle image holds it, so the composite is the same.
[shader("fragment")]
float4 fs_volume(VolumeVertex i) : SV_Target {
    // The ray: the eye-relative frame has no translation, so a direction d
    // projects to x = (row0 . d) / (row3 . d), y likewise; d is
    // perpendicular to row0 - x row3 and to row1 - y row3.
    const float3 r0 = push.ecef_to_clip[0].xyz;
    const float3 r1 = push.ecef_to_clip[1].xyz;
    const float3 r3 = push.ecef_to_clip[3].xyz;
    float3 dir = normalize(cross(r0 - i.ndc.x * r3, r1 - i.ndc.y * r3));
    if (dot(dir, r3) < 0.0) {
        dir = -dir;
    }
    const VolumeSpace v = volume_space[0];
    const float side = float(v.size) * v.voxel;
    const float3 corner = push.anchor + v.corner;  // from the eye
    float2 span = box_span(corner, side, dir);
    // Cut where the scene is nearer (as fs_particles: the nearest of the
    // scene's depths under this pixel at half resolution).
    uint depth_w;
    uint depth_h;
    scene_depth.GetDimensions(depth_w, depth_h);
    const int2 full = int2(int(depth_w), int(depth_h));
    const int2 here = int2(i.position.xy);
    int2 lo = here;
    int2 hi = here;
    if (push.depth_scale > 1) {
        const int2 half_size = (full + 1) / 2;
        lo = (here * full) / half_size;
        hi = ((here + 1) * full + half_size - 1) / half_size - 1;
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
        const float along = dot(r3, dir);  // view z per metre of ray
        span.y = min(span.y, near_distance() / depth / max(along, 1e-6));
    }
    if (span.y <= span.x) {
        discard;
    }
    // Steps of about a voxel, the first offset by a fixed per-pixel jitter.
    const int steps = clamp(int(ceil((span.y - span.x) * v.inv_voxel)), 1, 256);
    const float step = (span.y - span.x) / float(steps);
    uint h = uint(here.x) * 0x8da6b343u ^ uint(here.y) * 0xd8163841u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    const float jitter = float(h >> 8) * (1.0 / 16777216.0);
    const ViewSky sky = view_sky[lighting.view];
    const float g = push.forward_scattering;
    const Origin o = origins[0];
    // A pixel's width in metres per metre along the ray: the projection's
    // vertical scale (row 1 less its forward part, as vs_particles takes the
    // view's axes) over the view's height in pixels. The noise fades to its
    // mean where its features are under 4 pixels across, so a far cloud's
    // edge isn't hair (D-052: seen as fur at 20 000 puffs).
    const float3 forward = normalize(r3);
    const float scale_y = length(r1 - dot(r1, forward) * forward) / length(r3);
    // Metres a pixel of the particle image spans at 1 m (at half resolution
    // its pixels are two of the view's).
    const float m_per_pixel = 2.0 * push.inv_target.y * float(push.depth_scale) / max(scale_y, 1e-6);
    float through = 1.0;  // transmittance from the eye
    float3 light_sum = float3(0.0);
    for (int k = 0; k < steps; ++k) {
        const float t = span.x + (float(k) + jitter) * step;
        const float3 p = dir * t - push.anchor;  // from the anchor
        const float3 uvw = volume_uvw(p);
        float sigma = volume_density.SampleLevel(light_sampler, uvw, 0.0);
        if (sigma <= 0.0) {
            continue;
        }
        if (push.detail > 0.0) {
            const float3 off = p - o.offset;
            const float footprint = t * m_per_pixel;  // metres a pixel covers here
            const float fade = saturate(push.detail_size / (4.0 * max(footprint, 1e-4)));
            const float3 enu = float3(dot(off, o.east), dot(off, o.north), dot(off, o.up));
            const float n = lerp(kDetailMean, detail_at(enu), fade);
            sigma *= detail_factor(n);
            // The erosion (D-051) at the cloud's edge, not each puff's: the
            // cloud's own thickness here, as 1 - e^(-sigma x 2 m) (its core
            // near 1, its fringe near 0), less erosion x (1 - n), as a
            // puff's s is eroded. Thin dust where the field is low goes;
            // the core is kept.
            const float erosion = light_space[0].erosion;
            if (erosion > 0.0) {
                // The edge carved by the field at three times its size:
                // billows, not the fine detail's strands (which made a thick
                // cloud's edge look like fur).
                const float coarse = detail_at(enu * (1.0 / 3.0));
                // How deep in the cloud, against its particles' own
                // density (not per metre: thin dust was erased whole): about
                // 1 - r^2 through a lone particle, as D-051's s.
                const float s = 1.0 - exp(-2.0 * sigma * volume_space[0].inv_reference);
                const float t = erosion * (1.0 - coarse);
                sigma *= saturate((s - t) / max(1.0 - t, 1e-3) * 4.0) * light_space[0].erosion_gain;
            }
        }
        const float sun_depth = volume_sun_depth.SampleLevel(light_sampler, uvw, 0.0);
        const float3 light =
            sky.sun_irradiance * multiple_scattering_phase(g, dot(dir, lighting.sun), sun_depth) +
            sky.moon_irradiance * multiple_scattering_phase(g, dot(dir, lighting.moon), 0.0) +
            sky.sky_irradiance * (0.5 * kInvPi * kOctaveEnergy);
        // Hillaire 2016, equation 17: the step's scattering integrated
        // against its own extinction (the light scattered per metre is
        // sigma x albedo x light; over sigma, albedo x light).
        const float step_t = exp(-sigma * step);
        light_sum += through * (1.0 - step_t) * push.albedo * light;
        through *= step_t;
        if (through < 1e-3) {
            break;
        }
    }
    const float cover = 1.0 - through;
    if (cover < 1e-4) {
        discard;
    }
    return float4(min(light_sum * sky.exposure, float3(60000.0)), cover);
}

