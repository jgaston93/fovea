// The cloud layers (milestone 6 step 6, D-034): drawn after the resolve
// and the stars into the HDR target, one full-screen triangle a layer, the
// farthest first. Each pixel finds where its ray meets the layer's base
// (from below, or from above), and puts the cloud there, its depth tested
// against the resolved scene's so terrain nearer than it shows in front.
// The layer's coverage shapes procedural noise (weather.slang); it is lit
// by the sun, moon and sky at the eye, reflecting from above what it lets
// through below, and seen through the air and the fog.

#define FRAME_SET 0
#include "include/frame.slang"

// Mirrors CloudPush in clouds.cpp.
struct CloudPush {
    float4x4 clip_to_rel;  // the view's clip space to points relative to the eye (ECEF)
    float4x4 rel_to_clip;  // and back (SceneView's ecef_to_clip)
};
[[vk::push_constant]] ConstantBuffer<CloudPush> push;

struct CloudPixel {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 ndc;
    [[vk::location(1)]] nointerpolation uint layer;
};

struct CloudOut {
    float4 colour : SV_Target;
    float depth : SV_Depth;
};

[shader("vertex")]
CloudPixel vs_clouds(uint id: SV_VulkanVertexID, uint instance: SV_VulkanInstanceID) {
    // One triangle over the screen.
    const float2 ndc = float2(float((id << 1) & 2), float(id & 2)) * 2.0 - 1.0;
    CloudPixel o;
    o.position = float4(ndc, 0.0, 1.0);
    o.ndc = ndc;
    o.layer = instance;
    return o;
}

[shader("fragment")]
CloudOut fs_clouds(CloudPixel i) {
    const float4 l = lighting.layer[i.layer];
    // Any depth gives the direction; 1 is the near plane (reverse-Z).
    const float4 p = mul(push.clip_to_rel, float4(i.ndc, 1.0, 1.0));
    const float3 d = normalize(p.xyz / p.w);
    const float mu = dot(d, lighting.up);
    const float h = lighting.eye_height_m;
    // The base from below (the far root, ahead), or from above (the near
    // one, where the ray comes down to it).
    const float2 span = sphere_span(h, mu, l.x);
    const bool below = h < l.x;
    const float dist = below ? span.y : span.x;
    const bool hits = span.x <= span.y && dist > 0.0;
    // Its noise cells, and how many a pixel spans: taken for every pixel,
    // outside the branch (a pixel that misses gives its neighbours nothing
    // useful, which only fades their detail).
    const float3 rel = d * (hits ? dist : 0.0);
    const float3 cells = cloud_cells(rel, i.layer);
    const float footprint = max(length(ddx(cells)), length(ddy(cells)));
    if (!hits) {
        discard;
    }
    const float density = cloud_density(l.w, cells, 5, kCloudSpread, footprint);
    const float tau = cloud_depth(l) * density;
    // Opaque where it lets less than a thousandth through: NVIDIA blends a
    // half-float target in half floats, where 1 - alpha can't go below
    // about 5e-4, and the sun's disc behind (some 60,000) then shone through
    // even a full overcast. A thinner cloud dims the disc, as cloud does.
    const float through_sky = exp(-tau);
    const float alpha = through_sky < 1e-3 ? 1.0 : 1.0 - through_sky;
    if (alpha < 1e-3) {
        discard;
    }
    // The light on its top, as on level ground there; what it reflects up,
    // and scatters through down (less the light passing straight through:
    // the scene behind, which the blend keeps by 1 - alpha).
    const ViewSky s = view_sky[lighting.view];
    float3 up;
    (void)point_height(rel, up);
    const float3 top = s.sun_irradiance * saturate(dot(lighting.sun, up)) +
                       s.moon_irradiance * saturate(dot(lighting.moon, up)) + s.sky_irradiance;
    const float through = cloud_diffuse(tau);
    const float scattered = below ? max(through - exp(-tau), 0.0) : 1.0 - through;
    // Divided by alpha: the blend multiplies it back.
    float3 radiance = top * scattered * kInvPi / alpha;
    // Seen from below toward the sun, its thin edges glow with the light
    // scattered forward through them (single scattering, optical depth
    // tau: at most where tau is 1).
    if (below) {
        const float c = dot(d, lighting.sun);
        const float g = 0.6;
        const float phase = (1.0 - g * g) / (4.0 * kPi * pow(1.0 + g * g - 2.0 * g * c, 1.5));
        radiance += s.sun_irradiance * phase * tau * exp(-tau) / alpha;
    }
    // Through the air, then the fog, rain and snow, to the eye.
    const float4 air = aerial_perspective(i.position.xy, dist * 0.001);
    const float3 seen = through_bands(radiance * air.a + air_light(air, s), mu,
                                      dist, s, 1.0);
    CloudOut o;
    o.colour = float4(min(seen * s.exposure, float3(60000.0)) * alpha, alpha);
    const float4 clip = mul(push.rel_to_clip, float4(rel, 1.0));
    o.depth = saturate(clip.z / clip.w);
    return o;
}
