#pragma once

// Terrain databases on disk for tests: a few tiles whose heights follow a
// function exactly at their samples.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>

#include "fovea/terrain/database.hpp"
#include "fovea/terrain/tile_key.hpp"

namespace fovea::test {

struct TestDb {
    std::filesystem::path dir;
    DatabaseManifest manifest;
};

// Under the system temp directory, one folder per test run (so builds
// running at once don't share it), removed when the tests end.
const std::filesystem::path& test_root();

// A database named `name` under test_root() whose terrain follows
// `height(level, lat, lon)` exactly at its samples, with tiles at the given
// levels covering `area`.
TestDb make_db(const std::string& name, const GeoBounds& area,
               std::initializer_list<std::uint8_t> levels, std::uint32_t samples,
               const std::function<double(std::uint8_t, double, double)>& height);

std::unique_ptr<TerrainDatabase> open_db(const TestDb& t);

}  // namespace fovea::test
