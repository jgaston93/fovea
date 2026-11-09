#pragma once

// The database's frame (D-037): tiles are laid out in WGS-84 (G2139)
// geographic coordinates at an epoch, with heights above its ellipsoid. The
// sources (3DEP, NAIP, the FAA's survey) are NAD83(2011), fixed to the
// North American plate; WGS-84 moves with ITRF, about 1.2 m horizontally
// and 1.3 m vertically apart at KHSV in 2026.
//
// PROJ (through GDAL) gives the shift: NAD83(2011) to ITRF2014 (the NGS's
// time-dependent Helmert) to WGS-84 (G2139), stated to 1 cm. It is taken on
// a grid over the area and interpolated, as it varies by well under a
// millimetre a kilometre. With datum.cpp, elevation.cpp and imagery.cpp
// the only files that see GDAL's API.

#include <optional>
#include <string>
#include <vector>

#include "fovea/terrain/tile_key.hpp"

namespace fovea::compiler {

struct Nad83Point {
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    // Added to a NAD83(2011) ellipsoid height there, the WGS-84 one.
    double height_shift_m = 0.0;
};

class DatumShift {
public:
    // Over `area` (WGS-84), at `epoch` (a decimal year). Nothing, with why,
    // if PROJ has no transformation better than its ballpark one (which
    // would leave the coordinates as they are).
    static std::optional<DatumShift> make(const GeoBounds& area, double epoch, std::string& error);

    // None: the sources' frame taken as WGS-84, as before D-037 (tests of
    // other things use it).
    static DatumShift none();

    // A WGS-84 point in NAD83(2011).
    Nad83Point to_nad83(double lat_deg, double lon_deg) const;
    // A NAD83(2011) point in WGS-84 (the FAA's runway ends, placements).
    void to_wgs84(double& lat_deg, double& lon_deg) const;

    double epoch() const { return epoch_; }

private:
    DatumShift() = default;

    // The shift (NAD83 less WGS-84 latitude and longitude, degrees, and the
    // height shift) at each grid point, row 0 at the south.
    struct Shift {
        double lat = 0.0;
        double lon = 0.0;
        double height = 0.0;
    };
    Shift at(double lat_deg, double lon_deg) const;

    GeoBounds area_;
    std::size_t rows_ = 0;
    std::size_t columns_ = 0;
    std::vector<Shift> grid_;
    double epoch_ = 0.0;
};

}  // namespace fovea::compiler
