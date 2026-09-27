# The renderer (SDL3 GPU) and its shaders.

include(FetchContent)

# SDL3: an installed package if there is one, else the pinned release.
find_package(SDL3 3.2 CONFIG QUIET)
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

# opengp_shader(<source> <header name> <array name> [glslang args...])
set(shader_out ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(shader_headers)
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
endfunction()

opengp_shader(tev.vert tev_vert kTevVert)
opengp_shader(tev.frag tev_frag kTevFrag)
opengp_shader(tev.frag tev_frag_early kTevFragEarly -DEARLY_Z)
opengp_shader(fullscreen.vert fullscreen_vert kFullscreenVert)
opengp_shader(copy.frag copy_frag kCopyFrag)
opengp_shader(color.frag color_frag kColorFrag)
opengp_shader(blit.frag blit_frag kBlitFrag)

add_library(opengp_video STATIC renderer.cpp png.cpp ${shader_headers})
target_include_directories(opengp_video PRIVATE ${shader_out})
target_link_libraries(opengp_video
  PUBLIC opengp_machine SDL3::SDL3
  PRIVATE opengp_warnings)
