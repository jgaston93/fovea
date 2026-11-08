# Views, presenters, modules and the Vulkan backend (src/vk). Reads
# SceneState only; never includes net headers. Draws terrain databases
# (libs/terrain) it is given.
add_library(fovea_render STATIC
    src/display_clock.cpp
    src/frame_loop.cpp
    src/resolve.cpp
    src/scene_view.cpp
    src/terrain_cache.cpp
    src/terrain_lod.cpp
    src/view.cpp
    src/vk/models.cpp
    src/vk/overlay_ui.cpp
    src/vk/renderer.cpp
    src/vk/sky.cpp
    src/vk/stars.cpp
    src/vk/terrain.cpp
    src/vk/vma.cpp)
add_library(fovea::render ALIAS fovea_render)
target_include_directories(fovea_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_render
    PUBLIC fovea::core fovea::platform fovea::terrain fovea::models
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
# Models (milestone 4 step 3).
set(model_shader "${PROJECT_SOURCE_DIR}/shaders/model.slang")
fovea_add_shader(fovea_render "${model_shader}" vs_model vertex kVsModel ${terrain_capabilities})
fovea_add_shader(fovea_render "${model_shader}" fs_model fragment kFsModel ${terrain_capabilities})
# The weighted resolve of multisampled scenes (milestone 5 step 1).
fovea_add_shader(fovea_render "${PROJECT_SOURCE_DIR}/shaders/resolve.slang" fs_resolve fragment
                 kFsResolve)
# The sky's tables and each view's light (milestone 6 step 2).
set(sky_shader "${PROJECT_SOURCE_DIR}/shaders/sky.slang")
fovea_add_shader(fovea_render "${sky_shader}" cs_transmittance compute kCsTransmittance)
fovea_add_shader(fovea_render "${sky_shader}" cs_multi_scattering compute kCsMultiScattering)
fovea_add_shader(fovea_render "${sky_shader}" cs_sky_view compute kCsSkyView)
fovea_add_shader(fovea_render "${sky_shader}" cs_irradiance compute kCsIrradiance)
fovea_add_shader(fovea_render "${sky_shader}" cs_exposure compute kCsExposure)
fovea_add_shader(fovea_render "${sky_shader}" cs_aerial compute kCsAerial)
# The stars (milestone 6 step 4).
fovea_add_shader(fovea_render "${PROJECT_SOURCE_DIR}/shaders/stars.slang" vs_stars vertex kVsStars)
fovea_add_shader(fovea_render "${PROJECT_SOURCE_DIR}/shaders/stars.slang" fs_stars fragment kFsStars)
# Just the generated shader headers: what clang-tidy needs from a build.
add_custom_target(fovea_shaders DEPENDS ${FOVEA_SHADER_HEADERS})
