#include "vk_pipelines.h"

#include "vk_initializers.h"

#include "shader_bundle.gen.h"

bool vkutil::load_shader_module(const char* file_path, VkDevice device,
		VkShaderModule* out_shader_module) {
	BundleFileData shader_data{};
	bool shader_found = false;

	for (int i = 0; i < BUNDLE_FILE_COUNT; i++) {
		BundleFileData data = BUNDLE_FILES[i];
		if (data.path == file_path) {
			shader_data = data;
			shader_found = true;
			break;
		}
	}

	if (!shader_found) {
		return false;
	}

	// create a new shader module, using the buffer we loaded
	VkShaderModuleCreateInfo create_info = {};
	create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext = nullptr;

	// codeSize has to be in bytes, so multply the ints in the buffer by size of
	// int to know the real size of the buffer
	create_info.codeSize = shader_data.size;
	create_info.pCode = (uint32_t*)&BUNDLE_DATA[shader_data.start_idx];

	// check that the creation goes well.
	VkShaderModule shader_module;
	if (vkCreateShaderModule(device, &create_info, nullptr, &shader_module) !=
			VK_SUCCESS) {
		return false;
	}

	*out_shader_module = shader_module;
	return true;
}
