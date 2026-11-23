#pragma once

// A six-axis 3D mouse (D-058), read through SDL's raw HID access: SDL's
// joystick layer has no driver for one, and on Linux the kernel gives it
// relative axes, which SDL takes for neither a joystick nor a mouse.
//
// Found by its HID usage, Generic Desktop's Multi-axis Controller (page 0x01,
// usage 0x08), from 3Dconnexion's two vendor IDs (Logitech's 0x046d for the
// older devices, 3Dconnexion's 0x256f). Its input reports, as spacenavd
// (src/dev.c) and spacemouse-rs (src/report.rs) read them:
//   report 1, 7 bytes: x, y, z; 13 bytes: x, y, z, rx, ry, rz
//   report 2, 7 bytes: rx, ry, rz
//   report 3: the buttons, a little-endian bit mask (bit n: button n+1)
// each axis a signed little-endian 16-bit count, about +-350 at full
// deflection (the SpaceNavigator's descriptor; pyspacemouse's scale). HID's
// axes: x right, y toward the user, z down; rx, ry, rz right-handed about
// them (pyspacemouse's AxisConvention.HID).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace fovea {

inline constexpr unsigned short kHidUsagePageGenericDesktop = 0x01;
inline constexpr unsigned short kHidUsageMultiAxisController = 0x08;
inline constexpr double kSpaceMouseFullScale = 350.0;

// 3Dconnexion's vendor and a Multi-axis Controller: a 3D mouse.
bool is_space_mouse(unsigned short vendor_id, unsigned short usage_page, unsigned short usage);

// The device's raw counts: x, y, z, rx, ry, rz; the buttons held.
struct SpaceMouseRaw {
    std::array<int, 6> axes{};
    std::uint32_t buttons = 0;
    // A report 2 seen: rotation comes apart, so a long report 1 is padding
    // after the translation (Windows reads every report at the device's
    // longest report's length, zero-filled).
    bool rotation_apart = false;
};

// One input report (its ID first) into `raw`. False, and `raw` unchanged:
// a report it doesn't read (another ID, too short).
bool apply_space_mouse_report(std::span<const std::uint8_t> report, SpaceMouseRaw& raw);

struct SpaceMouseState {
    bool connected = false;
    std::array<double, 6> axes{};  // HID's, each -1 to 1 (a count over kSpaceMouseFullScale)
    std::uint32_t buttons = 0;     // held
    std::uint32_t pressed = 0;     // gone down since the last poll
};

// The reports since the last take, as a state: the axes and buttons as the
// last left them; `pressed`, each button that went down in any of them (so
// a press and release between two takes still counts).
class SpaceMouseReports {
public:
    void apply(std::span<const std::uint8_t> report);
    SpaceMouseState take();

private:
    SpaceMouseRaw raw_;
    std::uint32_t pressed_ = 0;
};

// The first 3D mouse found, read without blocking. None found (or it can't
// be opened, on Linux without read access to its hidraw node): looked for
// again when SDL sees a device come or go (at most every 2 s), and every
// 30 s whatever it sees; unplugged: its axes back to 0 and looked for again.
// Main thread.
class SpaceMouse {
public:
    SpaceMouse();
    ~SpaceMouse();
    SpaceMouse(const SpaceMouse&) = delete;
    SpaceMouse& operator=(const SpaceMouse&) = delete;

    // Every report waiting, then the state.
    SpaceMouseState poll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fovea
