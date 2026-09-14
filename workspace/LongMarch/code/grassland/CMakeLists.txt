add_library(Grassland grassland.cpp)

set(GRASSLAND_SUBLIB_NAME grassland_util)
add_subdirectory(util)
set(GRASSLAND_SUBLIB_NAME grassland_imgui)
add_subdirectory(imgui)
set(GRASSLAND_SUBLIB_NAME grassland_vulkan)
add_subdirectory(vulkan)
set(GRASSLAND_SUBLIB_NAME grassland_graphics)
add_subdirectory(graphics)
set(GRASSLAND_SUBLIB_NAME grassland_math)
add_subdirectory(math)

target_link_libraries(Grassland PUBLIC
        grassland_util
        grassland_imgui
        grassland_vulkan
        grassland_graphics
        grassland_math)
