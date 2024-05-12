set(VMA_BUILD_SAMPLES OFF CACHE BOOL "Build samples" FORCE)
add_subdirectory(VulkanMemoryAllocator)

add_subdirectory(fastgltf)

set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "Test Vk-Bootstrap using Catch2 as well as build examples" FORCE)
add_subdirectory(vk-bootstrap)
