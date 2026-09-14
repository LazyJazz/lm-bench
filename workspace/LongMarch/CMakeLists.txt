cmake_minimum_required(VERSION 3.25)

if (VCPKG_PATH)
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif ()

if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
            "You must set CMAKE_TOOLCHAIN_FILE to your vcpkg script.\n"
            "You may use VCPKG_PATH pointing to your vcpkg root directory for convenience.")
endif ()

project(LongMarch LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
endif ()

find_package(fmt CONFIG REQUIRED)
set(FMT_LIB_NAME fmt::fmt)
find_package(Eigen3 CONFIG REQUIRED)
set(EIGEN3_LIB_NAME Eigen3::Eigen)
find_package(glfw3 CONFIG REQUIRED)
set(GLFW3_LIB_NAME glfw)

find_package(Vulkan REQUIRED COMPONENTS dxc)
set(LONGMARCH_VULKAN_ENABLED ON)
set(VULKAN_LIB_NAME ${Vulkan_LIBRARIES})
set(DXC_LIB_NAME Vulkan::dxc_lib)
get_filename_component(VULKAN_DXC_DIR "${Vulkan_dxc_EXECUTABLE}" DIRECTORY)
get_filename_component(DXC_DLL_DIR "${Vulkan_dxc_LIBRARY}" DIRECTORY)

find_package(VulkanMemoryAllocator CONFIG REQUIRED)
set(VMA_LIB_NAME Vulkan::Headers GPUOpen::VulkanMemoryAllocator)
find_package(glm CONFIG REQUIRED)
set(GLM_LIB_NAME glm::glm)

add_library(imgui)
target_sources(imgui PRIVATE
        external/imgui/imgui.cpp
        external/imgui/imgui_draw.cpp
        external/imgui/imgui_widgets.cpp
        external/imgui/imgui_tables.cpp)
target_include_directories(imgui PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/external/imgui>
        $<INSTALL_INTERFACE:include>)
set(IMGUI_LIB_NAME imgui)

find_package(mikktspace CONFIG REQUIRED)
set(MIKKTSPACE_LIB_NAME mikktspace::mikktspace)
find_package(tinyobjloader CONFIG REQUIRED)
set(TINYOBJLOADER_LIB_NAME tinyobjloader::tinyobjloader)
find_package(RapidJSON CONFIG REQUIRED)
find_package(Stb REQUIRED)
set(STB_INC_DIR ${Stb_INCLUDE_DIR})

function(LONGMARCH_DEPLOY_RUNTIME_DEPENDENCIES TARGET_NAME)
    if (Vulkan_dxc_LIBRARY AND EXISTS "${Vulkan_dxc_LIBRARY}")
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${Vulkan_dxc_LIBRARY}" "$<TARGET_FILE_DIR:${TARGET_NAME}>"
                COMMENT "Deploying DXC runtime for ${TARGET_NAME}")
    endif ()

    if (UNIX AND NOT APPLE AND Vulkan_LIBRARY)
        file(REAL_PATH "${Vulkan_LIBRARY}" VULKAN_RUNTIME_LIBRARY)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VULKAN_RUNTIME_LIBRARY}" "$<TARGET_FILE_DIR:${TARGET_NAME}>/libvulkan.so.1"
                COMMENT "Deploying Vulkan runtime for ${TARGET_NAME}")
        set_property(TARGET ${TARGET_NAME} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        set_property(TARGET ${TARGET_NAME} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    endif ()
endfunction()

set(LONGMARCH_IMGUI_DIR ${CMAKE_CURRENT_LIST_DIR}/external/imgui)
set(LONGMARCH_ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/)
set(LONGMARCH_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/code)

add_subdirectory(scripts)
include(scripts/pack_shaders.cmake)
add_subdirectory(code)

if ("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}" AND NOT LONGMARCH_NO_DEMO)
    add_subdirectory(demo EXCLUDE_FROM_ALL)
endif ()
