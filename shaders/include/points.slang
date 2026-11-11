// How bright a point of light shows (D-038): the stars and the airport lights
// (milestone 6 step 8). A point's light, spread over a pixel, is far more
// than a display can show at night's exposure: a 2,500 cd light 5 km off
// gives the eye about 1e-4 lux, a thousand times what takes a pixel to
// white, so every star and light showed at full white whatever its
// intensity. Instead, a point shows by the square root of its light over
// a white point, on the display's own scale: twenty times less light shows
// about 4.5 times dimmer, and only points at least as bright as the white
// point reach white. The light is exposed first (D-031), so by day it's
// lost against the sky as before.

// The exposed illuminance (illuminance in the scene's units times the
// exposure) at which a point shows white: about what a runway edge light
// (2,500 cd) gives from 3.3 km, head on, at a moonless night's exposure.
static const float kPointWhite = 0.05;

// The ACES fit's inverse (reference_scene.slang's aces()): the value the
// tone map takes to `y`, below the curve's top (2.51 / 2.43).
float aces_inverse(float y) {
    const float a = 2.51 - 2.43 * y;
    const float b = 0.03 - 0.59 * y;
    const float c = -0.14 * y;
    return (-b + sqrt(b * b - 4.0 * a * c)) / (2.0 * a);
}

// The pre-exposed radiance to add to a pixel for a point giving it
// `exposed` illuminance (its light's colour, exposed, over its pixels):
// what the tone map takes to the point's brightness, with its hue.
float3 point_radiance(float3 exposed) {
    const float m = max(exposed.r, max(exposed.g, exposed.b));
    if (!(m > 0.0)) {
        return float3(0.0);
    }
    const float shown = min(sqrt(m / kPointWhite), 0.98);
    return exposed * (aces_inverse(shown) / m);
}
