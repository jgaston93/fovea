# Views, presenters, modules and the Vulkan backend (src/vk). Reads
# SceneState only; never includes net headers. Draws terrain databases
# (libs/terrain) it is given.
add_library(fovea_render STATIC
    src/display_clock.cpp
    src/frame_loop.cpp
    src/scene_view.cpp
    src/terrain_cache.cpp
    src/terrain_lod.cpp
    src/view.cpp
    src/vk/overlay_ui.cpp
    src/vk/renderer.cpp
    src/vk/terrain.cpp
    src/vk/vma.cpp)
add_library(fovea::render ALIAS fovea_render)
target_include_directories(fovea_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_render
    PUBLIC fovea::core fovea::platform fovea::terrain
    PRIVATE Vulkan::Headers volk::volk vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator
            fovea_imgui)
# Vulkan functions come from volk (no prototypes, no static linking to the
# loader); VMA takes them from volk too.
target_compile_definitions(fovea_render PRIVATE
    VK_NO_PROTOTYPES VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
fovea_target_defaults(fovea_render)

# The reference scene's shaders (step 6), compiled with slangc and embedded.
set(scene_shader "${PROJECT_SOURCE_DIR}/shaders/reference_scene.slang")
fovea_add_shader(fovea_render "${scene_shader}" vs_fullscreen vertex kVsFullscreen)
fovea_add_shader(fovea_render "${scene_shader}" fs_scene fragment kFsScene)
fovea_add_shader(fovea_render "${scene_shader}" fs_tonemap fragment kFsTonemap)
# Terrain (milestone 3 step 5).
set(terrain_shader "${PROJECT_SOURCE_DIR}/shaders/terrain.slang")
# NonUniformResourceIndex makes slangc widen the profile with texture
# capabilities the SPIR-V doesn't use (it declares only ShaderNonUniform and
# the descriptor-array ones): named here, so the widening isn't a warning.
set(terrain_capabilities -capability
    "SPV_GOOGLE_user_type+spvDerivativeControl+spvImageQuery+spvImageGatherExtended+spvSparseResidency+spvMinLod+spvFragmentFullyCoveredEXT")
fovea_add_shader(fovea_render "${terrain_shader}" vs_terrain vertex kVsTerrain
                 ${terrain_capabilities})
fovea_add_shader(fovea_render "${terrain_shader}" fs_terrain fragment kFsTerrain
                 ${terrain_capabilities})
# Just the generated shader headers: what clang-tidy needs from a build.
add_custom_target(fovea_shaders DEPENDS ${FOVEA_SHADER_HEADERS})
