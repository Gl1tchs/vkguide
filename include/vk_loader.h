#pragma once

#include "vk_types.h"

#include <filesystem>
#include <unordered_map>

struct GeoSurface {
	uint32_t start_index;
	uint32_t count;
};

struct MeshAsset {
	std::string name;

	std::vector<GeoSurface> surfaces;
	GPUMeshBuffers mesh_buffers;
};

// forward declaration
class VulkanEngine;

std::optional<std::vector<std::shared_ptr<MeshAsset>>> load_gltf_meshes(
		VulkanEngine* engine, std::filesystem::path file_path);
