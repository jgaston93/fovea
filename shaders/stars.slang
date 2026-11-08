// The stars (milestone 6 step 4): the Bright Star Catalogue's 9,096, drawn
// after the resolve into the HDR target as single-pixel points (D-028:
// lights smaller than a pixel aren't multisampled), where the resolved
// depth is still the sky's (0, reverse-Z), so terrain and models hide them.
// Each star puts its illuminance, through the air, into its pixel: bright
// against a night sky, lost in a day sky by the exposure alone.

#include "include/atmosphere.slang"
#define FRAME_SET 1
#include "include/frame.slang"
[[vk::binding(3, 1)]] ConstantBuffer<Atmosphere> atmosphere;

// Right ascension and declination (J2000, degrees), V magnitude, B-V.
[[vk::binding(0, 0)]] StructuredBuffer<float4> stars;

// Mirrors StarPush in stars.cpp.
struct StarPush {
    float4x4 j2000_to_clip;  // directions in J2000 to the view's clip space
    float3 up;               // the eye's up, in J2000
    float inv_pixel_solid_angle;
};
[[vk::push_constant]] ConstantBuffer<StarPush> push;

struct StarVertex {
    float4 position : SV_Position;
    float size : SV_PointSize;
    [[vk::location(0)]] float3 colour;  // its radiance in its pixel, before exposure
};

// What the fragment shader takes of it (the point's size is the vertex
// stage's alone).
struct StarPixel {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 colour;
};

// A star's colour from its B-V, as a rough blackbody's in linear sRGB,
// scaled to unit luminance (its magnitude sets its brightness).
float3 star_colour(float bv) {
    const float3 blue = float3(0.62, 0.72, 1.0);   // B-V -0.3
    const float3 white = float3(0.93, 0.95, 1.0);  // 0.3
    const float3 yellow = float3(1.0, 0.92, 0.80); // 0.8
    const float3 red = float3(1.0, 0.70, 0.45);    // 1.6
    float3 c;
    if (bv < 0.3) {
        c = lerp(blue, white, saturate((bv + 0.3) / 0.6));
    } else if (bv < 0.8) {
        c = lerp(white, yellow, (bv - 0.3) / 0.5);
    } else {
        c = lerp(yellow, red, saturate((bv - 0.8) / 0.8));
    }
    return c / luminance(c);
}

[shader("vertex")]
StarVertex vs_stars(uint id: SV_VulkanVertexID) {
    const float4 s = stars[id];
    const float ra = radians(s.x);
    const float dec = radians(s.y);
    const float3 dir = float3(cos(dec) * cos(ra), cos(dec) * sin(ra), sin(dec));
    StarVertex o;
    o.size = 1.0;
    const float mu = dot(dir, push.up);
    // Below the horizon, which dips below level as the eye climbs: outside
    // the view.
    const float dip = sqrt(max(1.0 - (atmosphere.bottom_radius / lighting.eye_radius) *
                                         (atmosphere.bottom_radius / lighting.eye_radius),
                               0.0));
    if (mu <= -dip) {
        o.position = float4(2.0, 2.0, 0.0, 1.0);
        o.colour = float3(0.0);
        return o;
    }
    // At infinity: w 0 in, depth 0 out (reverse-Z's far plane).
    o.position = mul(push.j2000_to_clip, float4(dir, 0.0));
    // Its illuminance outside the atmosphere: magnitude 0 is 2.0e-11 of the
    // sun's (-26.74), and five magnitudes are a factor of 100.
    const float e = atmosphere.sun_illuminance.g * pow(10.0, -0.4 * (s.z + 26.74));
    const float3 through =
        transmittance_table.SampleLevel(transmittance_uv(atmosphere, lighting.eye_radius, mu), 0.0)
            .rgb;
    // A pixel off the view's axis spans less sky (cos^3 of the angle off
    // it, which w is for a unit direction): its star is brighter there, so
    // the star shows the same on either side of a seam between displays.
    const float w = max(o.position.w, 1e-3);
    // Through the fog, rain and snow too (step 6).
    o.colour = star_colour(s.w) * e * through * band_transmittance(mu, 1e9) *
               push.inv_pixel_solid_angle / (w * w * w);
    return o;
}

[shader("fragment")]
float4 fs_stars(StarPixel i) : SV_Target {
    const ViewSky s = view_sky[lighting.view];
    return float4(min(i.colour * s.exposure, float3(60000.0)), 0.0);
}
