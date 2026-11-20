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
// toward the sun. Shadows within the cloud and from the terrain aren't drawn
// yet.
//
// Particles aren't sorted: each is composited over what's behind it
// ("over", premultiplied). Particles of one colour composite to the same
// result in any order; their colours differ only by the angle to the sun,
// smoothly, so the order shows little (D-040).

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
[[vk::binding(0, 0)]] StructuredBuffer<Particle> particles;
[[vk::binding(1, 0)]] StructuredBuffer<Origin> origins;
// The view's resolved depth (reverse-Z, infinite far plane), read by pixel.
[[vk::binding(0, 2)]] Texture2D<float> scene_depth;

// Mirrors ParticlePush in particles.cpp.
struct ParticlePush {
    float4x4 ecef_to_clip;  // the view's, for points relative to the eye
    float3 anchor;          // the origins' anchor relative to the eye
    float forward_scattering;
    float3 albedo;
    float unused;
};
[[vk::push_constant]] ConstantBuffer<ParticlePush> push;

struct ParticleVertex {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 ray;  // the quad's point relative to the eye, ECEF axes
    [[vk::location(1)]] nointerpolation float3 centre;
    [[vk::location(2)]] nointerpolation float radius;
    [[vk::location(3)]] nointerpolation float extinction;
};

// The near plane's distance: with an infinite reverse-Z projection, clip z
// is the near distance for every point.
float near_distance() {
    return push.ecef_to_clip[2].w;
}

static const float2 kCorners[6] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
                                    float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0) };

[shader("vertex")]
ParticleVertex vs_particles(uint id: SV_VulkanVertexID) {
    const Particle p = particles[id / 6];
    const Origin o = origins[p.origin];
    const float2 corner = kCorners[id % 6];
    const float3 c = push.anchor + o.offset + o.east * p.position.x + o.north * p.position.y +
                     o.up * p.position.z;
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
//     overlapping puff, and the five left out add an eighth of the light;
//   - no a: the light's path through the cloud isn't traced (no
//     self-shadowing yet), so its transmittance is taken as 1 for every
//     octave, and a has nothing to scale.
// So it isn't energy-conserving (Hillaire, page 41, requires a <= b for
// that): the octaves sum to 1.75 times the light once scattered, and an
// albedo over 1/1.75 scatters more than falls on the puff. The look is
// tuned by [particles]' albedo, not derived (D-040).
float multiple_scattering_phase(float g, float c) {
    float sum = 0.0;
    float energy = 1.0;
    for (int k = 0; k < 3; ++k) {
        sum += energy * mie_phase(g, c);
        energy *= 0.5;
        g *= 0.5;
    }
    return sum;
}
static const float kOctaveEnergy = 1.75;  // b^0 + b^1 + b^2: the octaves' sum for an isotropic light

[shader("fragment")]
float4 fs_particles(ParticleVertex i) : SV_Target {
    const float3 dir = normalize(i.ray);
    // Where the ray enters and leaves the sphere, in floats: Haines, Guenther
    // and Akenine-Moeller, "Precision Improvements for Ray/Sphere
    // Intersection", Ray Tracing Gems (2019), chapter 7. The discriminant as
    // r^2 - l^2 (their equation 5, after Hearn and Baker), l the centre's
    // distance from the ray, not b^2 - (|c|^2 - r^2), which loses a small
    // puff far off; the roots as q and c/q (their equation 10, after Press et
    // al.), not b -/+ sqrt, which loses the nearer one when the eye is close
    // to a large puff's surface. Here the centre is relative to the eye and
    // the ray's direction is a unit vector, so their b' is b and a is 1.
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
    // Cut off where the scene is nearer: depth 0 is the sky.
    const float depth = scene_depth.Load(int3(int2(i.position.xy), 0));
    if (depth > 0.0) {
        const float view_z = near_distance() / depth;
        const float along = dot(push.ecef_to_clip[3].xyz, dir);  // view z per metre of ray
        leave = min(leave, view_z / max(along, 1e-6));
    }
    const float length_inside = leave - enter;
    if (length_inside <= 0.0) {
        discard;
    }
    const float opacity = 1.0 - exp(-i.extinction * length_inside);
    if (opacity < 1e-4) {
        discard;
    }
    // The light it scatters toward the eye: the sun's and the moon's by the
    // phase function, the sky's as if from the whole upper hemisphere, each
    // scattered several times (multiple_scattering_phase).
    const ViewSky sky = view_sky[lighting.view];
    const float g = push.forward_scattering;
    const float3 light =
        sky.sun_irradiance * multiple_scattering_phase(g, dot(dir, lighting.sun)) +
        sky.moon_irradiance * multiple_scattering_phase(g, dot(dir, lighting.moon)) +
        sky.sky_irradiance * (0.5 * kInvPi * kOctaveEnergy);
    const float3 radiance = push.albedo * light * sky.exposure;
    return float4(min(radiance, float3(60000.0)) * opacity, opacity);
}
