# Compiles Slang shaders to SPIR-V at build time and embeds the words in the
# binary, so nothing is read from disk at run time (D-003). SPIR-V is a
# build output, never committed.
#
#   fovea_add_shader(<target> <source.slang> <entry> <stage> <name>)
#
# makes <name>.hpp (in the target's generated include directory) with
#   inline constexpr std::array<std::uint32_t, N> <name> = {...};
# Matrices use column-major layout, like glm, so a glm matrix in a push
# constant means the same matrix in the shader. Each header is also added to
# FOVEA_SHADER_HEADERS in the caller's scope, for a target that builds just
# the shaders (clang-tidy needs the headers without a full build).

set(FOVEA_SHADER_EMBED "${CMAKE_CURRENT_LIST_DIR}/EmbedSpirv.cmake")

function(fovea_add_shader target source entry stage name)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
    set(spv "${out_dir}/${name}.spv")
    set(hpp "${out_dir}/${name}.hpp")
    add_custom_command(
        OUTPUT "${hpp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND "${FOVEA_SLANGC}" "${source}" -target spirv -profile spirv_1_6
                -entry ${entry} -stage ${stage} -matrix-layout-column-major
                -warnings-as-errors all ${ARGN} -o "${spv}"
        COMMAND ${CMAKE_COMMAND} -DSPV=${spv} -DHPP=${hpp} -DNAME=${name}
                -P "${FOVEA_SHADER_EMBED}"
        DEPENDS "${source}" "${FOVEA_SHADER_EMBED}"
        COMMENT "slangc ${name} (${entry}, ${stage})"
        VERBATIM)
    target_sources(${target} PRIVATE "${hpp}")
    set(FOVEA_SHADER_HEADERS ${FOVEA_SHADER_HEADERS} "${hpp}" PARENT_SCOPE)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endfunction()
