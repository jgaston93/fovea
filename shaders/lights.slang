// Airport lights (milestone 6 step 8, D-033): the database's lights layer,
// drawn after the resolve into the HDR target as points at least a pixel
// across (D-028: a light smaller than a pixel isn't multisampled), tested
// against the resolved depth. Each puts its illuminance at the eye into its
// pixels: its intensity toward the eye by its beam, over the distance
// squared, through the air, the fog, rain and snow. Its colour is the one
// the layer gives for the side the eye is on, and a PAPI unit's is white
// above its angle and red below.
//
// The intensities and beams are approximate: AC 150/5340-30J, which the
// layouts come from, gives positions and colours, while the fixtures'
// photometry is in AC 150/5345-46, not used yet. They're about what
// high-intensity fixtures give at a middle brightness step at night.

#include "include/atmosphere.slang"
#define FRAME_SET 1
#include "include/frame.slang"
[[vk::binding(3, 1)]] ConstantBuffer<Atmosphere> atmosphere;

// Mirrors GpuLight in lights.cpp.
struct Light {
    float3 offset;  // from the lights' origin, ECEF axes, metres
    uint packed;    // type, then colour[0] and colour[1], a byte each
    float3 axis;    // the runway's direction there (from its first end), level, ECEF
    float papi_tan; // a PAPI unit's angle, as its tangent; 0 for other lights
    float3 up;      // the vertical there, ECEF
    float unused;
};
[[vk::binding(0, 0)]] StructuredBuffer<Light> lights;

// Mirrors LightPush in lights.cpp.
struct LightPush {
    float4x4 ecef_to_clip;  // the view's, for points relative to the eye
    float3 origin;          // the lights' origin relative to the eye
    float pixel_angle;      // a pixel's width on the view's axis, radians
    float inv_pixel_solid_angle;
    float max_size;         // the largest point size the GPU draws, pixels
};
[[vk::push_constant]] ConstantBuffer<LightPush> push;

// The layer's types (tile_file.hpp's LightType) and colours (LightColour).
static const uint kEdge = 0;
static const uint kThreshold = 1;
static const uint kCentreline = 2;
static const uint kTouchdownZone = 3;
static const uint kPapi = 4;

// Light on level ground in the scene's units: frame.slang's sun gives
// 10,000 of them for 100,000 lux.
static const float kUnitsPerLux = 0.1;

// A colour's light, by the filter that makes it, as a fraction of the
// white lamp's, in linear sRGB: the filters pass less than the lamp's
// whole light (red and green least).
float3 light_colour(uint c) {
    switch (c) {
    case 1:
        return float3(1.0, 0.86, 0.62);  // white: a halogen lamp, warm
    case 2:
        return float3(1.0, 0.62, 0.08) * 0.55;  // yellow
    case 3:
        return float3(1.0, 0.04, 0.02) * 0.9;  // red (its luminance is 0.23 of white's)
    case 4:
        return float3(0.02, 1.0, 0.55) * 0.35;  // aviation green, blue-green
    default:
        return float3(0.0);
    }
}

// Candela on the beam's axis, by type.
float peak_candela(uint type) {
    switch (type) {
    case kEdge:
    case kThreshold:
        return 2500.0;
    case kCentreline:
    case kTouchdownZone:
        return 1000.0;
    default:
        return 6000.0;  // PAPI
    }
}

// How far a fixture stands above the ground: elevated edge and threshold
// lights and PAPI units, and the in-pavement ones nearly flush.
float lift_m(uint type) {
    return type == kCentreline || type == kTouchdownZone ? 0.05 : 0.35;
}

struct LightVertex {
    float4 position : SV_Position;
    float size : SV_PointSize;
    [[vk::location(0)]] float3 colour;  // its radiance in each of its pixels, before exposure
    [[vk::location(1)]] nointerpolation float round;  // 1: a disc, 0: the whole square
};

struct LightPixel {
    float4 position : SV_Position;
    float2 coord : SV_PointCoord;
    [[vk::location(0)]] float3 colour;
    [[vk::location(1)]] nointerpolation float round;
};

LightVertex hidden() {
    LightVertex o;
    o.position = float4(2.0, 2.0, 0.0, 1.0);  // outside the view
    o.size = 1.0;
    o.colour = float3(0.0);
    o.round = 0.0;
    return o;
}

[shader("vertex")]
LightVertex vs_lights(uint id: SV_VulkanVertexID) {
    const Light l = lights[id];
    const uint type = l.packed & 0xFFu;
    const float3 rel = push.origin + l.offset + l.up * lift_m(type);
    const float dist = max(length(rel), 0.1);
    const float3 to_eye = -rel / dist;

    // Which side: an aircraft travelling along the axis sees colour[0],
    // so the eye is behind the light along it.
    const float elevation = dot(to_eye, l.up);  // the sine of the eye's angle above the light's level
    const float3 level = to_eye - l.up * elevation;
    const float level_length = length(level);
    const float along = level_length > 1e-4 ? dot(level, l.axis) / level_length : 1.0;
    const uint colour = (l.packed >> (along < 0.0 ? 8u : 16u)) & 0xFFu;
    if (colour == 0u || elevation < -0.01) {
        return hidden();  // dark that way, or below its ground
    }

    // The beam: along the runway, within about 10 to 15 degrees of it
    // across, and from just above level to about 10 degrees up, where an
    // approach and a landing see it. Edge lights also glow all round
    // (their omnidirectional part), dimmer, for circling.
    const float across = acos(clamp(abs(along), 0.0, 1.0));
    const float up_angle = asin(clamp(elevation, -1.0, 1.0));
    const float spread = type == kEdge || type == kThreshold ? radians(15.0) : radians(10.0);
    float beam = exp(-(across * across) / (spread * spread));
    beam *= exp(-pow((up_angle - radians(4.0)) / radians(7.0), 2.0));
    if (type == kEdge) {
        beam = max(beam, 0.05);
    }
    float3 tint = light_colour(colour);
    if (type == kPapi) {
        // White above the unit's angle, red below, across a few minutes
        // of arc: the eye's angle above the unit's level.
        const float tan_up = elevation / max(level_length, 1e-4);
        const float t = saturate((tan_up - l.papi_tan) / radians(3.0 / 60.0) + 0.5);
        tint = lerp(light_colour(3), light_colour(1), t);
    }
    const float lux = peak_candela(type) * beam / (dist * dist);

    // Through the air (step 3) and the fog, rain and snow (step 6) to the
    // eye; none of it under rain's or snow's overcast is lost to it.
    LightVertex o;
    o.position = mul(push.ecef_to_clip, float4(rel, 1.0));
    if (o.position.w <= 0.0) {
        return hidden();
    }
    const float2 uv = o.position.xy / o.position.w * 0.5 + 0.5;
    const float air = aerial_perspective_uv(uv, dist * 0.001).a;
    const float mu = dot(rel, lighting.up) / dist;
    const float through = air * band_transmittance(mu, dist);

    // Its pixels: at least one, or as many as a 0.4 m fixture covers, the
    // light shared among them. A pixel off the view's axis spans less of
    // the view (cos^3 of the angle off it, cos = w / dist), so a light
    // shows the same on either side of a seam between displays.
    const float off_axis = o.position.w / dist;
    const float pixel = push.pixel_angle * off_axis * sqrt(off_axis);
    const float size = clamp(0.4 / (dist * pixel), 1.0, push.max_size);
    o.size = size;
    o.round = size > 2.5 ? 1.0 : 0.0;
    const float area = size * size * (o.round > 0.5 ? 0.785398 : 1.0);
    o.colour = tint * lux * kUnitsPerLux * through * push.inv_pixel_solid_angle /
               (off_axis * off_axis * off_axis * area);

    // Tested against the terrain nearer than it stands, moved along the
    // line to the eye so it stays on the same pixels: the resolved depth
    // is a pixel's nearest, and seen at a grazing angle a pixel of ground
    // reaches far nearer than its middle (from 6 m up, a pixel at 450 m
    // spans 380 to 550 m). Its reach toward the eye, over the light's own
    // ground: the pixel's width over the sine of the angle the eye looks
    // down at it (from the light, the eye's elevation); and far off the
    // ground may be drawn coarse, a little above it. No more than half the
    // way: a hill between the eye and the light still hides it.
    const float grazing = 2.0 * size * dist * pixel / max(elevation, 1e-3);
    const float margin = min(1.0 + 0.02 * dist + grazing, 0.5 * dist);
    const float4 nearer = mul(push.ecef_to_clip, float4(rel * (1.0 - margin / dist), 1.0));
    o.position.z = min(nearer.z / nearer.w, 1.0) * o.position.w;  // not past the near plane
    return o;
}

[shader("fragment")]
float4 fs_lights(LightPixel i) : SV_Target {
    if (i.round > 0.5 && length(i.coord - 0.5) > 0.5) {
        discard;
    }
    const ViewSky s = view_sky[lighting.view];
    return float4(min(i.colour * s.exposure, float3(60000.0)), 0.0);
}
