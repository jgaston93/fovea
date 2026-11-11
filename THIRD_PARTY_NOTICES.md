# Third-party notices

Fovea's own code is under the Apache License 2.0 (`LICENSE`). It builds with the libraries below, fetched by CMake at the pinned versions in `cmake/FoveaDependencies.cmake`; none are committed here. Each license was read from the library's own license file at that version (2026-11-10). All are permissive and compatible with Apache-2.0.

Binaries of Fovea that include these libraries must carry their copyright and license notices: the MIT, zlib and BSD terms ask for it. Map data is not code; its terms and credits are in `ATTRIBUTION.md`.

## In `fovea` (the runtime) and its libraries

| Library | Version | License | Copyright | Used for |
|---|---|---|---|---|
| [SDL](https://github.com/libsdl-org/SDL) | 3.4.18 | zlib | Sam Lantinga | windows, monitors, input |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | SDK 1.4.363.0 | Apache-2.0 or MIT | The Khronos Group Inc. | Vulkan API headers |
| [volk](https://github.com/zeux/volk) | SDK 1.4.363.0 | MIT | Arseny Kapoulkine | Vulkan loader |
| [vk-bootstrap](https://github.com/charles-lunarg/vk-bootstrap) | 1.4.363 | MIT | Charles Giessen | Vulkan setup |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | 3.4.0 | MIT | Advanced Micro Devices, Inc. | GPU memory |
| [GLM](https://github.com/g-truc/glm) | 1.0.1 | MIT (or the Happy Bunny License) | G-Truc Creation | maths |
| [toml++](https://github.com/marzer/tomlplusplus) | 3.4.0 | MIT | Mark Gillard | config files |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93.0 | MIT | Omar Cornut | the timing overlay; it embeds the ProggyClean and ProggyForever fonts (MIT; Tristan Grimmer, Disco Hello) |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | Johannes Kuhlmann | glTF models |
| [stb_image, stb_image_write](https://github.com/nothings/stb) | commit a7d4c21 | MIT or public domain (Unlicense), our choice: MIT | Sean Barrett | image files |

## In `fovea-compiler` only

| Library | Version | License | Copyright | Used for |
|---|---|---|---|---|
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (`bc7enc.cpp`, `bc7decomp.cpp`, `rgbcx.cpp`) | commit b943862 | MIT or public domain (Unlicense), our choice: MIT | Richard Geldreich, Jr. | BC7 and BC1 texture encoding. The repository's `bc7e.ispc` (Apache-2.0) and LodePNG aren't used. |
| [GDAL](https://gdal.org) and [PROJ](https://proj.org) | the system's (3.8 or later) | MIT | their authors | reading sources, reprojection. Linked from the system, not distributed with Fovea. |

## Build and test tools (not in Fovea's binaries)

| Tool | Version | License | Used for |
|---|---|---|---|
| [Slang](https://github.com/shader-slang/slang) (`slangc`, prebuilt) | 2026.17.1 | Apache-2.0 WITH LLVM-exception | compiling shaders to SPIR-V |
| [GoogleTest](https://github.com/google/googletest) | 1.15.2 | BSD-3-Clause | the tests |

## Reference documents (fetched, not distributed)

`tools/fetch-cigi-icd.sh` and `tools/fetch-faa-ac.sh` download the CIGI 3.3 ICD and its errata, and the FAA's AC 150/5340-30J, into `docs/reference/`, which git ignores. Fovea's code is written from them and cites them; it doesn't include or redistribute them.
