#include "vk_pipelines.h"

#include "vk_initializers.h"

#include "shader_bundle.gen.h"

#include <vulkan/vulkan_core.h>

void PipelineBuilder::clear() {
	_input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	};

	_rasterizer = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	};

	_color_blend_attachment = {};

	_multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	};

	_pipeline_layout = {};

	_depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
	};

	_render_info = { .sType =
							 VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };

	_shader_stages.clear();
}

VkPipeline PipelineBuilder::build_pipeline(VkDevice device) {
	// make viewport state from our stored viewport and scissor.
	// at the moment we wont support multiple viewports or scissors
	VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.pNext = nullptr,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	// setup dummy color blending. We arent using transparent objects yet
	// the blending is just "no blend", but we do write to the color attachment
	VkPipelineColorBlendStateCreateInfo color_blending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.pNext = nullptr,
		.logicOpEnable = VK_FALSE,
		.logicOp = VK_LOGIC_OP_COPY,
		.attachmentCount = 1,
		.pAttachments = &_color_blend_attachment,
	};

	// completely clear VertexInputStateCreateInfo, as we have no need for it
	VkPipelineVertexInputStateCreateInfo vertex_input_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	// create dynamic state
	VkDynamicState state[] = { VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR };

	VkPipelineDynamicStateCreateInfo dynamic_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = &state[0],
	};

	// build the actual pipeline
	// we now use all of the info structs we have been writing into into this
	// one to create the pipeline
	VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		// connect the renderInfo to the pNext extension mechanism
		.pNext = &_render_info,
		.stageCount = (uint32_t)_shader_stages.size(),
		.pStages = _shader_stages.data(),
		.pVertexInputState = &vertex_input_info,
		.pInputAssemblyState = &_input_assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &_rasterizer,
		.pMultisampleState = &_multisampling,
		.pDepthStencilState = &_depth_stencil,
		.pColorBlendState = &color_blending,
		.pDynamicState = &dynamic_info,
		.layout = _pipeline_layout,
	};

	// its easy to error out on create graphics pipeline, so we handle it a bit
	// better than the common VK_CHECK case
	VkPipeline new_pipeline;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
				nullptr, &new_pipeline) != VK_SUCCESS) {
		fmt::println("Failed to create pipeline!");
		return VK_NULL_HANDLE;
	}

	return new_pipeline;
}

void PipelineBuilder::set_layout(VkPipelineLayout layout) {
	_pipeline_layout = layout;
}

void PipelineBuilder::set_shaders(
		VkShaderModule vertex_shader, VkShaderModule fragment_shader) {
	_shader_stages.clear();

	_shader_stages.push_back(vkinit::pipeline_shader_stage_create_info(
			VK_SHADER_STAGE_VERTEX_BIT, vertex_shader));

	_shader_stages.push_back(vkinit::pipeline_shader_stage_create_info(
			VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader));
}

void PipelineBuilder::set_input_topology(VkPrimitiveTopology topology) {
	_input_assembly.topology = topology;
	// we are not going to use primitive restart on the entire tutorial so leave
	// it on false
	_input_assembly.primitiveRestartEnable = false;
}

void PipelineBuilder::set_polygon_mode(VkPolygonMode mode) {
	_rasterizer.polygonMode = mode;
	_rasterizer.lineWidth = 1.0f;
}

void PipelineBuilder::set_cull_mode(
		VkCullModeFlags cull_mode, VkFrontFace front_face) {
	_rasterizer.cullMode = cull_mode;
	_rasterizer.frontFace = front_face;
}

void PipelineBuilder::set_multisampling_none() {
	_multisampling.sampleShadingEnable = VK_FALSE;
	// multisampling defaulted to no multisampling (1 sample per pixel)
	_multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	_multisampling.minSampleShading = 1.0f;
	_multisampling.pSampleMask = nullptr;
	//no alpha to coverage either
	_multisampling.alphaToCoverageEnable = VK_FALSE;
	_multisampling.alphaToOneEnable = VK_FALSE;
}

void PipelineBuilder::disable_blending() {
	// default write mask
	_color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
			VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT;
	// no blending
	_color_blend_attachment.blendEnable = VK_FALSE;
}

void PipelineBuilder::set_color_attachment_format(VkFormat format) {
	_color_attachment_format = format;

	// connect the format to the renderInfo  structure
	_render_info.colorAttachmentCount = 1;
	_render_info.pColorAttachmentFormats = &_color_attachment_format;
}

void PipelineBuilder::set_depth_format(VkFormat format) {
	_render_info.depthAttachmentFormat = format;
}

void PipelineBuilder::disable_depthtest() {
	_depth_stencil.depthTestEnable = VK_FALSE;
	_depth_stencil.depthWriteEnable = VK_FALSE;
	_depth_stencil.depthCompareOp = VK_COMPARE_OP_NEVER;
	_depth_stencil.depthBoundsTestEnable = VK_FALSE;
	_depth_stencil.stencilTestEnable = VK_FALSE;
	_depth_stencil.front = {};
	_depth_stencil.back = {};
	_depth_stencil.minDepthBounds = 0.f;
	_depth_stencil.maxDepthBounds = 1.f;
}

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
