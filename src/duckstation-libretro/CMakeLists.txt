# The libretro core: DuckStation as one shared library (duckstation_libretro), rendering with Vulkan on the frontend's
# device. DUCKSTATION_LIBRETRO_INCLUDE_DIR names the folder with libretro.h and libretro_vulkan.h.
if(NOT DUCKSTATION_LIBRETRO_INCLUDE_DIR)
  message(FATAL_ERROR "Set DUCKSTATION_LIBRETRO_INCLUDE_DIR to the folder with libretro.h and libretro_vulkan.h")
endif()

add_library(duckstation_libretro SHARED
  libretro_core.cpp
)
target_include_directories(duckstation_libretro PRIVATE "${DUCKSTATION_LIBRETRO_INCLUDE_DIR}")
target_link_libraries(duckstation_libretro PRIVATE core util common scmversion)

if(NOT MSVC)
  # Only retro_* is visible; with -Bsymbolic the core's own symbols always bind inside it.
  target_link_options(duckstation_libretro PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map" "-Wl,-Bsymbolic" "-Wl,--no-undefined")
endif()
if(ANDROID)
  target_link_options(duckstation_libretro PRIVATE "-Wl,-z,max-page-size=16384")
  target_link_libraries(duckstation_libretro PRIVATE log android)
endif()
