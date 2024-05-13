cmake_minimum_required(VERSION 3.25)

project(vkguide)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)
find_package(fmt REQUIRED)
find_package(glm REQUIRED)

add_subdirectory(vendor)

file(GLOB_RECURSE SOURCES "include/*.h" "src/*.cpp")

set(IMGUI_DIR vendor/imgui1.90)
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.h"
    "${IMGUI_DIR}/imgui_internal.h"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.h"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.h"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp"
    "${IMGUI_DIR}/misc/cpp/imgui_stdlib.h"
    "${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp"
)

add_executable(vkguide ${SOURCES} ${IMGUI_SOURCES})

target_include_directories(vkguide PRIVATE 
    include/ {Vulkan_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${fmt_INCLUDE_DIRS}
    ${glm_INCLUDE_DIRS}
    vendor/VulkanMemoryAllocator/include
    vendor/fastgltf/include
    ${IMGUI_DIR}
    vendor/vk-bootstrap/src
    vendor/stb
    ${CMAKE_BINARY_DIR}/include
)

target_link_libraries(vkguide PRIVATE
    ${Vulkan_LIBRARIES}
    SDL2::SDL2
    fmt::fmt
    GPUOpen::VulkanMemoryAllocator
    fastgltf
    vk-bootstrap::vk-bootstrap
)
