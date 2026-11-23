#include "fovea/platform/space_mouse.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>

#include <SDL3/SDL_hidapi.h>

namespace fovea {

namespace {

constexpr unsigned short kVendorLogitech = 0x046d;  // the older 3Dconnexion devices
constexpr unsigned short kVendor3Dconnexion = 0x256f;

int le16(std::span<const std::uint8_t> r, std::size_t at) {
    return static_cast<std::int16_t>(
        static_cast<std::uint16_t>(r[at] | static_cast<unsigned>(r[at + 1]) << 8U));
}

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; w != nullptr && *w != L'\0'; ++w) {
        s.push_back(*w >= 0x20 && *w < 0x7f ? static_cast<char>(*w) : '?');
    }
    return s;
}

}  // namespace

bool is_space_mouse(unsigned short vendor_id, unsigned short usage_page, unsigned short usage) {
    return (vendor_id == kVendorLogitech || vendor_id == kVendor3Dconnexion) &&
           usage_page == kHidUsagePageGenericDesktop && usage == kHidUsageMultiAxisController;
}

bool apply_space_mouse_report(std::span<const std::uint8_t> report, SpaceMouseRaw& raw) {
    if (report.empty()) {
        return false;
    }
    switch (report[0]) {
        case 1:  // translation, or all six together
            if (report.size() < 7) {
                return false;
            }
            for (std::size_t i = 0; i < 3; ++i) {
                raw.axes[i] = le16(report, 1 + 2 * i);
            }
            if (report.size() >= 13) {
                for (std::size_t i = 0; i < 3; ++i) {
                    raw.axes[3 + i] = le16(report, 7 + 2 * i);
                }
            }
            return true;
        case 2:  // rotation
            if (report.size() < 7) {
                return false;
            }
            for (std::size_t i = 0; i < 3; ++i) {
                raw.axes[3 + i] = le16(report, 1 + 2 * i);
            }
            return true;
        case 3: {  // buttons: as many bytes of the mask as came, up to four
            if (report.size() < 2) {
                return false;
            }
            std::uint32_t mask = 0;
            const std::size_t n = std::min<std::size_t>(report.size() - 1, 4);
            for (std::size_t i = 0; i < n; ++i) {
                mask |= static_cast<std::uint32_t>(report[1 + i]) << (8U * i);
            }
            raw.buttons = mask;
            return true;
        }
        default:
            return false;
    }
}

SpaceMouseState space_mouse_state(const SpaceMouseRaw& raw, std::uint32_t previous_buttons) {
    SpaceMouseState s;
    s.connected = true;
    for (std::size_t i = 0; i < 6; ++i) {
        s.axes[i] = std::clamp(static_cast<double>(raw.axes[i]) / kSpaceMouseFullScale, -1.0, 1.0);
    }
    s.buttons = raw.buttons;
    s.pressed = raw.buttons & ~previous_buttons;
    return s;
}

struct SpaceMouse::Impl {
    SDL_hid_device* device = nullptr;
    SpaceMouseRaw raw;
    std::uint32_t last_buttons = 0;
    std::chrono::steady_clock::time_point next_look;
    bool told_none = false;

    void look() {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_look) {
            return;
        }
        next_look = now + std::chrono::seconds(2);
        SDL_hid_device_info* all = SDL_hid_enumerate(0, 0);
        bool found_but_closed = false;
        for (const SDL_hid_device_info* d = all; d != nullptr; d = d->next) {
            if (!is_space_mouse(d->vendor_id, d->usage_page, d->usage)) {
                continue;
            }
            device = SDL_hid_open_path(d->path);
            if (device == nullptr) {
                found_but_closed = true;
                continue;
            }
            (void)SDL_hid_set_nonblocking(device, 1);
            raw = {};
            last_buttons = 0;
            std::printf("3D mouse: %s (%04x:%04x)\n", narrow(d->product_string).c_str(),
                        unsigned{d->vendor_id}, unsigned{d->product_id});
            (void)std::fflush(stdout);
            told_none = false;
            break;
        }
        SDL_hid_free_enumeration(all);
        if (device == nullptr && found_but_closed && !told_none) {
            std::printf(
                "3D mouse: found but can't be opened (on Linux: read access to its "
                "hidraw node, a udev rule)\n");
            (void)std::fflush(stdout);
            told_none = true;
        }
    }
};

SpaceMouse::SpaceMouse() : impl_(std::make_unique<Impl>()) {
    impl_->look();
}

SpaceMouse::~SpaceMouse() {
    if (impl_->device != nullptr) {
        (void)SDL_hid_close(impl_->device);
    }
}

SpaceMouseState SpaceMouse::poll() {
    Impl& m = *impl_;
    if (m.device == nullptr) {
        m.look();
        if (m.device == nullptr) {
            return {};
        }
    }
    std::array<std::uint8_t, 64> buf{};
    for (int i = 0; i < 64; ++i) {  // every report waiting, within reason
        const int n = SDL_hid_read(m.device, buf.data(), buf.size());
        if (n == 0) {
            break;
        }
        if (n < 0) {  // unplugged: stop, and look again
            (void)SDL_hid_close(m.device);
            m.device = nullptr;
            m.next_look = {};
            std::printf("3D mouse: lost\n");
            (void)std::fflush(stdout);
            return {};
        }
        (void)apply_space_mouse_report(std::span(buf.data(), static_cast<std::size_t>(n)), m.raw);
    }
    const SpaceMouseState s = space_mouse_state(m.raw, m.last_buttons);
    m.last_buttons = m.raw.buttons;
    return s;
}

}  // namespace fovea
