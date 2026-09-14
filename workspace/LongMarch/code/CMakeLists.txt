add_subdirectory(grassland)
add_subdirectory(sparkium)

add_library(LongMarch INTERFACE)
target_include_directories(LongMarch INTERFACE ${LONGMARCH_INCLUDE_DIR})
target_link_libraries(LongMarch INTERFACE Grassland Sparkium)
