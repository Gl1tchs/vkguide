#pragma once

#include "vk_types.h"

class PipelineBuilder {
public:
	PipelineBuilder() { clear(); }

	void clear();

	VkPipeline build_pipeline(VkDevice device);

	void set_layout(VkPipelineLayout layout);

	void set_shaders(
			VkShaderModule vertex_shader, VkShaderModule fragment_shader);

	void set_input_topology(VkPrimitiveTopology topology);

	void set_polygon_mode(VkPolygonMode mode);

	void set_cull_mode(VkCullModeFlags cull_mode, VkFrontFace front_face);

	void set_multisampling_none();

	void disable_blending();

	void enable_blending_additive();

	void enable_blending_alphablend();

	void set_color_attachment_format(VkFormat format);

	void set_depth_format(VkFormat format);

	void enable_depthtest(bool depth_write_enable, VkCompareOp op);

	void disable_depthtest();

private:
	std::vector<VkPipelineShaderStageCreateInfo> _shader_stages;

	VkPipelineInputAssemblyStateCreateInfo _input_assembly;
	VkPipelineRasterizationStateCreateInfo _rasterizer;
	VkPipelineColorBlendAttachmentState _color_blend_attachment;
	VkPipelineMultisampleStateCreateInfo _multisampling;
	VkPipelineLayout _pipeline_layout;
	VkPipelineDepthStencilStateCreateInfo _depth_stencil;
	VkPipelineRenderingCreateInfo _render_info;
	VkFormat _color_attachment_format;
};

namespace vkutil {

bool load_shader_module(const char* file_path, VkDevice device,
		VkShaderModule* out_shader_module);

}
