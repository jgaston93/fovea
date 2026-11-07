// stb_image and stb_image_write compiled once, for image_file.cpp: PNG and
// JPEG (model textures, D-024). Third-party
// code: hidden from clang-tidy's static analyzer, which reports findings
// inside stb (it compiles everything else as usual).
#ifndef __clang_analyzer__
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO  // image_file.cpp reads files itself, through std::filesystem
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#endif
