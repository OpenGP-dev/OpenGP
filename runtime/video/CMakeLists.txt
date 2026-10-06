# The renderer (SDL3 GPU) and its shaders.

include(FetchContent)

# SDL3: an installed package if there is one, else the pinned release
# (always for the installer package, which ships it as a static library).
# GLOBAL: the input and audio libraries in runtime/ link it too.
if(NOT OPENGP_PACKAGE)
  find_package(SDL3 3.2 CONFIG QUIET GLOBAL)
endif()
if(NOT SDL3_FOUND)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
    URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(SDL3)
endif()

# glslang compiles the GLSL shaders to SPIR-V at build time: an installed
# glslangValidator (e.g. from the Vulkan SDK) if found, else built from the
# pinned release.
find_program(OPENGP_GLSLANG NAMES glslangValidator)
if(OPENGP_GLSLANG)
  set(glslang_command ${OPENGP_GLSLANG})
  set(glslang_depends)
else()
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
  set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glslang
    URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
    URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(glslang)
  set(glslang_command $<TARGET_FILE:glslang-standalone>)
  set(glslang_depends glslang-standalone)
endif()

# Metal (macOS): the shaders are also converted to the Metal Shading Language
# with SPIRV-Cross, using the resource bindings SDL's Metal backend expects
# (textures, samplers and buffers numbered by their binding).
option(OPENGP_METAL_SHADERS "Also build the shaders as Metal Shading Language (SDL's Metal backend)" ${APPLE})
if(OPENGP_METAL_SHADERS)
  set(SPIRV_CROSS_CLI ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)  # the CLI needs the other backends
  FetchContent_Declare(spirv_cross
    URL https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz
    URL_HASH SHA256=97c910326afdd44d794ce8561326fa675fd1958b27142f03295403044d639639
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(spirv_cross)
endif()

# opengp_shader(<source> <header name> <array name> [glslang args...])
#
# SPIR-V in <header name>.h as `<array name>`; with OPENGP_METAL_SHADERS also
# MSL in <header name>_msl.cpp as `<array name>Msl` (and `<array name>Msl_size`).
set(shader_out ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(shader_headers)
set(shader_sources)
function(opengp_shader source name array)
  set(out ${shader_out}/${name}.h)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_out}
    COMMAND ${glslang_command} -V --target-env vulkan1.0 ${ARGN} --vn ${array} -o ${out}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source} ${glslang_depends}
    COMMENT "Compiling shader ${name}"
    VERBATIM)
  set(shader_headers ${shader_headers} ${out} PARENT_SCOPE)
  if(OPENGP_METAL_SHADERS)
    set(spv ${shader_out}/${name}.spv)
    set(msl ${shader_out}/${name}.metal)
    set(cpp ${shader_out}/${name}_msl.cpp)
    add_custom_command(
      OUTPUT ${cpp}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_out}
      COMMAND ${glslang_command} -V --target-env vulkan1.0 ${ARGN} -o ${spv}
              ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}
      COMMAND $<TARGET_FILE:spirv-cross> ${spv} --msl --msl-version 20100 --msl-decoration-binding
              --output ${msl}
      COMMAND ${CMAKE_COMMAND} -DINPUT=${msl} -DOUTPUT=${cpp} -DNAME=${array}Msl
              -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source} ${glslang_depends} spirv-cross
              ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
      COMMENT "Converting shader ${name} to Metal"
      VERBATIM)
    set(shader_sources ${shader_sources} ${cpp} PARENT_SCOPE)
  endif()
endfunction()

opengp_shader(tev.vert tev_vert kTevVert)
opengp_shader(tev.frag tev_frag kTevFrag)
opengp_shader(tev.frag tev_frag_early kTevFragEarly -DEARLY_Z)
opengp_shader(fullscreen.vert fullscreen_vert kFullscreenVert)
opengp_shader(copy.frag copy_frag kCopyFrag)
opengp_shader(color.frag color_frag kColorFrag)
opengp_shader(blit.frag blit_frag kBlitFrag)
opengp_shader(sample.frag sample_frag kSampleFrag)

add_library(opengp_video STATIC renderer.cpp png.cpp ${shader_headers} ${shader_sources})
target_include_directories(opengp_video PRIVATE ${shader_out})
if(OPENGP_METAL_SHADERS)
  target_compile_definitions(opengp_video PRIVATE OPENGP_METAL_SHADERS)
endif()
target_link_libraries(opengp_video
  PUBLIC opengp_machine SDL3::SDL3
  PRIVATE opengp_warnings)
