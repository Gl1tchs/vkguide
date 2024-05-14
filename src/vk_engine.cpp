#include "vk_engine.h"

#include "vk_descriptors.h"
#include "vk_images.h"
#include "vk_initializers.h"
#include "vk_loader.h"
#include "vk_pipelines.h"
#include "vk_types.h"

#include <SDL.h>
#include <SDL_video.h>
#include <SDL_vulkan.h>
#include <VkBootstrap.h>
#include <fmt/core.h>
#include <vulkan/vulkan_core.h>
#include <glm/packing.hpp>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <thread>

void GLTFMetallic_Roughness::build_pipeline(VulkanEngine* engine) {
	VkShaderModule mesh_frag_shader;
	if (!vkutil::load_shader_module(
				"mesh.frag.spv", engine->_device, &mesh_frag_shader)) {
		fmt::println("Error while building the mesh fragment shader.");
	}

	VkShaderModule mesh_vert_shader;
	if (!vkutil::load_shader_module(
				"mesh.vert.spv", engine->_device, &mesh_vert_shader)) {
		fmt::println("Error while building the mesh vertex shader.");
	}

	VkPushConstantRange matrix_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = sizeof(GPUDrawPushConstants),
	};

	DescriptorLayoutBuilder layout_builder;
	layout_builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	layout_builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	layout_builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

	material_layout = layout_builder.build(engine->_device,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	VkDescriptorSetLayout layouts[] = {
		engine->_gpu_scene_data_descriptor_layout,
		material_layout,
	};

	VkPipelineLayoutCreateInfo mesh_layout_info =
			vkinit::pipeline_layout_create_info();
	mesh_layout_info.setLayoutCount = 2;
	mesh_layout_info.pSetLayouts = layouts;
	mesh_layout_info.pushConstantRangeCount = 1,
	mesh_layout_info.pPushConstantRanges = &matrix_range;

	VkPipelineLayout new_layout;
	VK_CHECK(vkCreatePipelineLayout(
			engine->_device, &mesh_layout_info, nullptr, &new_layout));

	opaque_pipeline.layout = new_layout;
	transparent_pipeline.layout = new_layout;

	// build the stage-create-info for both vertex and fragment stages. This
	// lets
	// the pipeline know the shader modules per stage
	PipelineBuilder pipeline_builder;
	pipeline_builder.set_shaders(mesh_vert_shader, mesh_frag_shader);
	pipeline_builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipeline_builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipeline_builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipeline_builder.set_multisampling_none();
	pipeline_builder.disable_blending();
	pipeline_builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);

	// render format
	pipeline_builder.set_color_attachment_format(
			engine->_draw_image.image_format);
	pipeline_builder.set_depth_format(engine->_depth_image.image_format);

	// use the mesh layout we created
	pipeline_builder.pipeline_layout = new_layout;

	// finally build the pipeline
	opaque_pipeline.pipeline = pipeline_builder.build_pipeline(engine->_device);

	// create the transparent variant
	pipeline_builder.enable_blending_additive();
	pipeline_builder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);

	transparent_pipeline.pipeline =
			pipeline_builder.build_pipeline(engine->_device);

	vkDestroyShaderModule(engine->_device, mesh_frag_shader, nullptr);
	vkDestroyShaderModule(engine->_device, mesh_vert_shader, nullptr);
}

void GLTFMetallic_Roughness::clear_resources(VkDevice device) {}

MaterialInstance GLTFMetallic_Roughness::write_material(VkDevice device,
		MaterialPass pass, const MaterialResources& resources,
		DescriptorAllocatorGrowable& descriptor_allocator) {
	MaterialInstance mat_data;
	mat_data.pass_type = pass;
	switch (mat_data.pass_type) {
		case MaterialPass::Transparent:
			mat_data.pipeline = &transparent_pipeline;
			break;
		case MaterialPass::MainColor:
			mat_data.pipeline = &opaque_pipeline;
			break;
		default:
			mat_data.pipeline = &opaque_pipeline;
			break;
	}

	mat_data.material_set =
			descriptor_allocator.allocate(device, material_layout);

	writer.clear();
	writer.write_buffer(0, resources.data_buffer, sizeof(MaterialConstants),
			resources.data_buffer_offset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.write_image(1, resources.color_image.image_view,
			resources.color_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.write_image(2, resources.metal_roughness_image.image_view,
			resources.metal_roughness_sampler,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

	writer.update_set(device, mat_data.material_set);

	return mat_data;
}

void MeshNode::draw(const glm::mat4& top_matrix, DrawContext& ctx) {
	glm::mat4 node_matrix = top_matrix * world_transform;

	for (auto& s : mesh->surfaces) {
		RenderObject def = {
			.index_count = s.count,
			.first_index = s.start_index,
			.index_buffer = mesh->mesh_buffers.index_buffer.buffer,
			.material = &s.material->data,
			.transform = node_matrix,
			.vertex_buffer_address = mesh->mesh_buffers.vertex_buffer_address,
		};

		ctx.opaque_surfaces.push_back(def);
	}
}

constexpr bool USE_VALIDATION_LAYERS = false;

constexpr uint64_t ONE_SECOND_IN_NANOSECONDS = 1000000000;

VulkanEngine* loaded_engine = nullptr;

VulkanEngine& VulkanEngine::get() { return *loaded_engine; }

void VulkanEngine::init() {
	// only one engine initialization is allowed with the application.
	assert(loaded_engine == nullptr);
	loaded_engine = this;

	// We initialize SDL and create a window with it.
	SDL_Init(SDL_INIT_VIDEO);

	SDL_WindowFlags window_flags =
			(SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

	_window = SDL_CreateWindow("Vulkan Engine", SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED, _window_extent.width,
			_window_extent.height, window_flags);

	init_vulkan();

	init_swapchain();

	init_commands();

	init_sync_structures();

	init_descriptors();

	init_pipelines();

	init_imgui();

	init_default_data();

	// everything went fine
	_is_initialized = true;
}

void VulkanEngine::cleanup() {
	if (_is_initialized) {
		//make sure the gpu has stopped doing its things
		vkDeviceWaitIdle(_device);

		for (auto& frame : _frames) {
			frame.deletion_queue.flush();
		}

		_deletion_queue.flush();

		SDL_DestroyWindow(_window);
	}

	// clear engine pointer
	loaded_engine = nullptr;
}

void VulkanEngine::run() {
	SDL_Event e;
	bool quit = false;

	// main loop
	while (!quit) {
		// Handle events on queue
		while (SDL_PollEvent(&e) != 0) {
			// close the window when user alt-f4s or clicks the X button
			if (e.type == SDL_QUIT) {
				quit = true;
			}

			if (e.type == SDL_WINDOWEVENT) {
				if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
					_stop_rendering = true;
				}
				if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
					_stop_rendering = false;
				}
			}

			// send SDL event to imgui for handling
			ImGui_ImplSDL2_ProcessEvent(&e);
		}

		// do not draw if we are minimized
		if (_stop_rendering) {
			// throttle the speed to avoid the endless spinning
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		if (_resize_requested) {
			resize_swapchain();
		}

		// imgui new frame
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();

		if (ImGui::Begin("Background")) {
			ImGui::SliderFloat("Render Scale", &_render_scale, 0.3f, 1.f);

			ComputeEffect& selected =
					_background_effects[_current_background_effect];

			// draw combo box for shader selection
			{
				static auto get_effect_name = [](const ComputeEffect& effect) {
					return effect.name;
				};

				std::vector<const char*> effect_names(
						_background_effects.size());
				std::transform(_background_effects.begin(),
						_background_effects.end(), effect_names.begin(),
						get_effect_name);

				ImGui::Combo("Effect", &_current_background_effect,
						effect_names.data(), effect_names.size());
			}

			ImGui::InputFloat4("data1", (float*)&selected.data.data1);
			ImGui::InputFloat4("data2", (float*)&selected.data.data2);
			ImGui::InputFloat4("data3", (float*)&selected.data.data3);
			ImGui::InputFloat4("data4", (float*)&selected.data.data4);

			ImGui::End();
		}

		// make imgui calculate internal draw structures
		ImGui::Render();

		// our draw function
		draw();
	}
}

void VulkanEngine::update_scene() {
	_main_draw_context.opaque_surfaces.clear();

	_loaded_nodes["Suzanne"]->draw(glm::mat4(1.0f), _main_draw_context);

	_scene_data.view = glm::translate(glm::mat4(1.0f), glm::vec3{ 0, 0, -5 });
	// camera projection
	_scene_data.proj = glm::perspective(glm::radians(70.f),
			(float)_window_extent.width / (float)_window_extent.height, 0.1f,
			10000.f);

	// invert the Y direction on projection matrix so that we are more similar
	// to opengl and gltf axis
	_scene_data.proj[1][1] *= -1;
	_scene_data.viewproj = _scene_data.proj * _scene_data.view;

	//some default lighting parameters
	_scene_data.ambient_color = glm::vec4(0.1f);
	_scene_data.sunlight_color = glm::vec4(1.0f);
	_scene_data.sunlight_direction = glm::vec4(0, 1, 0.5, 1.f);
}

GPUMeshBuffers VulkanEngine::upload_mesh(
		std::span<uint32_t> indices, std::span<Vertex> vertices) {
	const size_t vertex_buffer_size = vertices.size() * sizeof(Vertex);
	const size_t index_buffer_size = indices.size() * sizeof(uint32_t);

	GPUMeshBuffers new_surface;

	// create vertex buffer
	new_surface.vertex_buffer = create_buffer(vertex_buffer_size,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
					VK_BUFFER_USAGE_TRANSFER_DST_BIT |
					VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VMA_MEMORY_USAGE_GPU_ONLY);

	// find the address of the vertex buffer
	VkBufferDeviceAddressInfo device_address_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = new_surface.vertex_buffer.buffer,
	};
	new_surface.vertex_buffer_address =
			vkGetBufferDeviceAddress(_device, &device_address_info);

	// create index buffer
	new_surface.index_buffer = create_buffer(index_buffer_size,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_GPU_ONLY);

	AllocatedBuffer staging = create_buffer(
			vertex_buffer_size + index_buffer_size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

	void* data = staging.allocation->GetMappedData();

	// copy vertex buffer
	memcpy(data, vertices.data(), vertex_buffer_size);
	// copy index buffer
	memcpy((char*)data + vertex_buffer_size, indices.data(), index_buffer_size);

	immediate_submit([&](VkCommandBuffer cmd) {
		VkBufferCopy vertex_copy{ 0 };
		vertex_copy.dstOffset = 0;
		vertex_copy.srcOffset = 0;
		vertex_copy.size = vertex_buffer_size;

		vkCmdCopyBuffer(cmd, staging.buffer, new_surface.vertex_buffer.buffer,
				1, &vertex_copy);

		VkBufferCopy index_copy{ 0 };
		index_copy.dstOffset = 0;
		index_copy.srcOffset = vertex_buffer_size;
		index_copy.size = index_buffer_size;

		vkCmdCopyBuffer(cmd, staging.buffer, new_surface.index_buffer.buffer, 1,
				&index_copy);
	});

	destroy_buffer(staging);

	return new_surface;
}

AllocatedImage VulkanEngine::create_image(VkExtent3D size, VkFormat format,
		VkImageUsageFlags usage, bool mipmapped) {
	AllocatedImage new_image = {
		.image_extent = size,
		.image_format = format,
	};

	VkImageCreateInfo img_info = vkinit::image_create_info(format, usage, size);
	if (mipmapped) {
		img_info.mipLevels = static_cast<uint32_t>(std::floor(std::log2(
									 std::max(size.width, size.height)))) +
				1;
	}

	// always allocate images on dedicated GPU memory
	VmaAllocationCreateInfo alloc_info = {
		.usage = VMA_MEMORY_USAGE_GPU_ONLY,
		.requiredFlags =
				VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};

	// allocate and create the image
	VK_CHECK(vmaCreateImage(_allocator, &img_info, &alloc_info,
			&new_image.image, &new_image.allocation, nullptr));

	// if the format is a depth format, we will need to have it use the correct
	// aspect flag
	VkImageAspectFlags aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT;
	if (format == VK_FORMAT_D32_SFLOAT) {
		aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT;
	}

	// build image view for the image
	VkImageViewCreateInfo view_info = vkinit::imageview_create_info(
			format, new_image.image, aspect_flags);
	view_info.subresourceRange.levelCount = img_info.mipLevels;

	VK_CHECK(vkCreateImageView(
			_device, &view_info, nullptr, &new_image.image_view));

	return new_image;
}

AllocatedImage VulkanEngine::create_image(void* data, VkExtent3D size,
		VkFormat format, VkImageUsageFlags usage, bool mipmapped) {
	size_t data_size = size.depth * size.width * size.height * 4;
	AllocatedBuffer staging_buffer = create_buffer(data_size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	memcpy(staging_buffer.info.pMappedData, data, data_size);

	AllocatedImage new_image = create_image(size, format,
			usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			mipmapped);

	immediate_submit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, new_image.image,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkBufferImageCopy copy_region = {
            .bufferOffset =0,
            .bufferRowLength = 0,
            .bufferImageHeight =0,
            .imageSubresource = {
                .aspectMask =VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer =0,
                .layerCount = 1,
            },
            .imageExtent =size,
        };

		// copy the buffer into the image
		vkCmdCopyBufferToImage(cmd, staging_buffer.buffer, new_image.image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

		vkutil::transition_image(cmd, new_image.image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	destroy_buffer(staging_buffer);

	return new_image;
}

void VulkanEngine::destroy_image(const AllocatedImage& img) {
	vkDestroyImageView(_device, img.image_view, nullptr);
	vmaDestroyImage(_allocator, img.image, img.allocation);
}

void VulkanEngine::init_default_data() {
	_test_meshes = load_gltf_meshes(this, "assets/basicmesh.glb").value();

	//3 default textures, white, grey, black. 1 pixel each
	uint32_t white = glm::packUnorm4x8(glm::vec4(1, 1, 1, 1));
	_white_image = create_image((void*)&white, VkExtent3D{ 1, 1, 1 },
			VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1));
	_grey_image = create_image((void*)&grey, VkExtent3D{ 1, 1, 1 },
			VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 0));
	_black_image = create_image((void*)&black, VkExtent3D{ 1, 1, 1 },
			VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	//checkerboard image
	uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
	std::array<uint32_t, 16 * 16> pixels; //for 16x16 checkerboard texture
	for (int x = 0; x < 16; x++) {
		for (int y = 0; y < 16; y++) {
			pixels[y * 16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
		}
	}
	_error_checkerboard_image =
			create_image(pixels.data(), VkExtent3D{ 16, 16, 1 },
					VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	VkSamplerCreateInfo sampl = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO
	};

	sampl.magFilter = VK_FILTER_NEAREST;
	sampl.minFilter = VK_FILTER_NEAREST;

	vkCreateSampler(_device, &sampl, nullptr, &_default_sampler_nearest);

	sampl.magFilter = VK_FILTER_LINEAR;
	sampl.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler(_device, &sampl, nullptr, &_default_sampler_linear);

	GLTFMetallic_Roughness ::MaterialResources material_resources;
	// default the material resources
	material_resources.color_image = _white_image;
	material_resources.color_sampler = _default_sampler_linear;
	material_resources.metal_roughness_image = _white_image;
	material_resources.metal_roughness_sampler = _default_sampler_linear;

	// set the uniform buffer for the material data
	AllocatedBuffer material_constants = create_buffer(
			sizeof(GLTFMetallic_Roughness::MaterialConstants),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	// write the buffer
	GLTFMetallic_Roughness::MaterialConstants* scene_uniform_data =
			(GLTFMetallic_Roughness::MaterialConstants*)
					material_constants.allocation->GetMappedData();
	scene_uniform_data->color_factors = glm::vec4(1, 1, 1, 1);
	scene_uniform_data->metal_rough_factors = glm::vec4(1, 0.5f, 0, 0);

	_deletion_queue.push_function(
			[=, this]() { destroy_buffer(material_constants); });

	material_resources.data_buffer = material_constants.buffer;
	material_resources.data_buffer_offset = 0;

	_default_data = _metal_rough_material.write_material(_device,
			MaterialPass::MainColor, material_resources,
			_global_descriptor_allocator);

	for (auto& m : _test_meshes) {
		std::shared_ptr<MeshNode> new_node = std::make_shared<MeshNode>();
		new_node->mesh = m;

		new_node->local_transform = glm::mat4{ 1.f };
		new_node->world_transform = glm::mat4{ 1.f };

		for (auto& s : new_node->mesh->surfaces) {
			s.material = std::make_shared<GLTFMaterial>(_default_data);
		}

		_loaded_nodes[m->name] = std::move(new_node);
	}

	_deletion_queue.push_function([this]() {
		for (auto& mesh : _test_meshes) {
			destroy_buffer(mesh->mesh_buffers.vertex_buffer);
			destroy_buffer(mesh->mesh_buffers.index_buffer);
		}

		// destroy default images
		destroy_image(_white_image);
		destroy_image(_black_image);
		destroy_image(_grey_image);
		destroy_image(_error_checkerboard_image);

		vkDestroySampler(_device, _default_sampler_linear, nullptr);
		vkDestroySampler(_device, _default_sampler_nearest, nullptr);
	});
}

void VulkanEngine::draw() {
	update_scene();

	// wait until the gpu has finished rendering the last frame. Timeout of 1
	// second
	VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame().render_fence,
			true, ONE_SECOND_IN_NANOSECONDS));

	get_current_frame().deletion_queue.flush();
	get_current_frame().frame_descriptors.clear_pools(_device);

	// request image from the swapchain
	uint32_t swapchain_image_index;
	VkResult e = vkAcquireNextImageKHR(_device, _swapchain,
			ONE_SECOND_IN_NANOSECONDS, get_current_frame().swapchain_semaphore,
			nullptr, &swapchain_image_index);
	if (e == VK_ERROR_OUT_OF_DATE_KHR) {
		_resize_requested = true;
		return;
	}

	// wait till we ensure that swapchain is not out of date
	VK_CHECK(vkResetFences(_device, 1, &get_current_frame().render_fence));

	// naming it cmd for shorter writing
	VkCommandBuffer cmd = get_current_frame().main_command_buffer;

	// now that we are sure that the commands finished executing, we can safely
	// reset the command buffer to begin recording again.
	VK_CHECK(vkResetCommandBuffer(cmd, 0));

	// begin the command buffer recording. We will use this command buffer
	// exactly once, so we want to let vulkan know that
	VkCommandBufferBeginInfo cmd_begin_info = vkinit::command_buffer_begin_info(
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	_draw_extent.width =
			std::min(_swapchain_extent.width, _draw_image.image_extent.width) *
			_render_scale;
	_draw_extent.height = std::min(_swapchain_extent.height,
								  _draw_image.image_extent.height) *
			_render_scale;

	// start the command buffer recording
	VK_CHECK(vkBeginCommandBuffer(cmd, &cmd_begin_info));
	{
		// make the swapchain image into writeable mode before rendering
		vkutil::transition_image(cmd, _draw_image.image,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		draw_background(cmd);

		vkutil::transition_image(cmd, _draw_image.image,
				VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		vkutil::transition_image(cmd, _depth_image.image,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

		draw_geometry(cmd);

		// transition the draw image and the swapchain image into their correct
		// transfer layouts
		vkutil::transition_image(cmd, _draw_image.image,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkutil::transition_image(cmd, _swapchain_images[swapchain_image_index],
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		// execute a copy from the draw image into the swapchain
		vkutil::copy_image_to_image(cmd, _draw_image.image,
				_swapchain_images[swapchain_image_index], _draw_extent,
				_swapchain_extent);

		// set swapchain image layout to Attachment Optimal so we can draw it
		vkutil::transition_image(cmd, _swapchain_images[swapchain_image_index],
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		// draw imgui into the swapchain image
		draw_imgui(cmd, _swapchain_image_views[swapchain_image_index]);

		// set swapchain image layout to present so we can show it on the screen
		vkutil::transition_image(cmd, _swapchain_images[swapchain_image_index],
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}
	// finalize the command buffer (we can no longer add commands, but it can
	// now be executed)
	VK_CHECK(vkEndCommandBuffer(cmd));

	// prepare the submission to the queue.
	// we want to wait on the _presentSemaphore, as that semaphore is signaled
	// when the swapchain is ready we will signal the _renderSemaphore, to
	// signal that rendering has finished

	VkCommandBufferSubmitInfo cmd_info =
			vkinit::command_buffer_submit_info(cmd);

	VkSemaphoreSubmitInfo wait_info = vkinit::semaphore_submit_info(
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,
			get_current_frame().swapchain_semaphore);
	VkSemaphoreSubmitInfo signal_info =
			vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
					get_current_frame().render_semaphore);

	VkSubmitInfo2 submit =
			vkinit::submit_info(&cmd_info, &signal_info, &wait_info);

	// submit command buffer to the queue and execute it.
	// _renderFence will now block until the graphic commands finish execution
	VK_CHECK(vkQueueSubmit2(
			_graphics_queue, 1, &submit, get_current_frame().render_fence));

	// prepare present
	// this will put the image we just rendered to into the visible window.
	// we want to wait on the _renderSemaphore for that,
	// as its necessary that drawing commands have finished before the image is
	// displayed to the user
	VkPresentInfoKHR present_info = {};
	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.pNext = nullptr;
	present_info.pSwapchains = &_swapchain;
	present_info.swapchainCount = 1;

	present_info.pWaitSemaphores = &get_current_frame().render_semaphore;
	present_info.waitSemaphoreCount = 1;

	present_info.pImageIndices = &swapchain_image_index;

	e = vkQueuePresentKHR(_graphics_queue, &present_info);
	if (e == VK_ERROR_OUT_OF_DATE_KHR) {
		_resize_requested = true;
	}

	// increase the number of frames drawn
	_frame_number++;
}

void VulkanEngine::draw_geometry(VkCommandBuffer cmd) {
	// begin a render pass  connected to our draw image
	VkRenderingAttachmentInfo color_attachment = vkinit::attachment_info(
			_draw_image.image_view, nullptr, VK_IMAGE_LAYOUT_GENERAL);
	VkRenderingAttachmentInfo depth_attachment = vkinit::depth_attachment_info(
			_depth_image.image_view, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

	VkRenderingInfo render_info = vkinit::rendering_info(
			_draw_extent, &color_attachment, &depth_attachment);
	vkCmdBeginRendering(cmd, &render_info);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _mesh_pipeline);

	// set dynamic viewport and scissor
	VkViewport viewport = {};
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = _draw_extent.width;
	viewport.height = _draw_extent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor = {};
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = _draw_extent.width;
	scissor.extent.height = _draw_extent.height;

	vkCmdSetScissor(cmd, 0, 1, &scissor);

	// prepare global uniform buffer
	AllocatedBuffer gpu_scene_data_buffer = create_buffer(sizeof(GPUSceneData),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	// add it to the deletion queue of this frame so it gets deleted once
	// its been used
	get_current_frame().deletion_queue.push_function(
			[=, this]() { destroy_buffer(gpu_scene_data_buffer); });

	// write the buffer
	GPUSceneData* scene_uniform_data =
			(GPUSceneData*)gpu_scene_data_buffer.allocation->GetMappedData();
	*scene_uniform_data = _scene_data;

	// create a descriptor set that binds that buffer and update it
	VkDescriptorSet global_descriptor =
			get_current_frame().frame_descriptors.allocate(
					_device, _gpu_scene_data_descriptor_layout);

	DescriptorWriter writer;
	writer.write_buffer(0, gpu_scene_data_buffer.buffer, sizeof(GPUSceneData),
			0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.update_set(_device, global_descriptor);

	for (const RenderObject& draw : _main_draw_context.opaque_surfaces) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				draw.material->pipeline->pipeline);

		// bind descriptor sets
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				draw.material->pipeline->layout, 0, 1, &global_descriptor, 0,
				nullptr);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				draw.material->pipeline->layout, 1, 1,
				&draw.material->material_set, 0, nullptr);

		// push constants
		GPUDrawPushConstants push_constants = {
			.world_matrix = draw.transform,
			.vertex_buffer = draw.vertex_buffer_address,
		};
		vkCmdPushConstants(cmd, draw.material->pipeline->layout,
				VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants),
				&push_constants);

		// draw
		vkCmdBindIndexBuffer(cmd, draw.index_buffer, 0, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(cmd, draw.index_count, 1, draw.first_index, 0, 0);
	}

	vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_background(VkCommandBuffer cmd) {
	// make a clear-color from frame number. This will flash with a 360 frame
	// period.
	VkClearColorValue clear_value;
	float flash = std::abs(std::sin(_frame_number / 360.0f));
	clear_value = { { flash, 0.0f, 1.0f - flash, 1.0f } };

	VkImageSubresourceRange clear_range =
			vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

	// clear image
	// vkCmdClearColorImage(cmd, _draw_image.image, VK_IMAGE_LAYOUT_GENERAL,
	//		 &clear_value, 1, &clear_range);

	ComputeEffect& effect = _background_effects[_current_background_effect];

	// bind the gradient drawing compute pipeline
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);

	// bind the descriptor set containing the draw image for the compute
	// pipeline
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
			_background_pipeline_layout, 0, 1, &_draw_image_descriptors, 0,
			nullptr);

	vkCmdPushConstants(cmd, _background_pipeline_layout,
			VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants),
			&effect.data);

	// execute the compute pipeline dispatch. We are using 16x16 workgroup size
	// so we need to divide by it
	vkCmdDispatch(cmd, std::ceil(_draw_extent.width / 16.0f),
			std::ceil(_draw_extent.height / 16.0f), 1);
}

void VulkanEngine::init_vulkan() {
	vkb::InstanceBuilder builder;

	auto inst_ret = builder.set_app_name("Example Vulkan Application")
							.use_default_debug_messenger()
							.require_api_version(1, 3, 0)
							.build();

	vkb::Instance vkb_inst = inst_ret.value();

	_instance = vkb_inst.instance;
	_debug_messenger = vkb_inst.debug_messenger;

	SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

	VkPhysicalDeviceVulkan13Features features{};
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features.dynamicRendering = true;
	features.synchronization2 = true;

	VkPhysicalDeviceVulkan12Features features12{};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12.bufferDeviceAddress = true;
	features12.descriptorIndexing = true;

	//use vkbootstrap to select a gpu.
	//We want a gpu that can write to the SDL surface and supports vulkan 1.3
	//with the correct features
	vkb::PhysicalDeviceSelector selector{ vkb_inst };
	vkb::PhysicalDevice physical_device =
			selector.set_minimum_version(1, 3)
					.set_required_features_13(features)
					.set_required_features_12(features12)
					.set_surface(_surface)
					.select()
					.value();

	//create the final vulkan device
	vkb::DeviceBuilder device_builder{ physical_device };
	vkb::Device vkb_device = device_builder.build().value();

	_device = vkb_device.device;
	_chosenGPU = physical_device.physical_device;

	_graphics_queue = vkb_device.get_queue(vkb::QueueType::graphics).value();
	_graphics_queue_family =
			vkb_device.get_queue_index(vkb::QueueType::graphics).value();

	// instance cleanup
	_deletion_queue.push_function([this]() {
		vkDestroySurfaceKHR(_instance, _surface, nullptr);
		vkDestroyDevice(_device, nullptr);

		vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
		vkDestroyInstance(_instance, nullptr);
	});

	VmaAllocatorCreateInfo allocator_info = {};
	allocator_info.physicalDevice = _chosenGPU;
	allocator_info.device = _device;
	allocator_info.instance = _instance;
	allocator_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	vmaCreateAllocator(&allocator_info, &_allocator);

	_deletion_queue.push_function(
			[this]() { vmaDestroyAllocator(_allocator); });
}

void VulkanEngine::init_swapchain() {
	create_swapchain(_window_extent.width, _window_extent.height);

	_deletion_queue.push_function([this]() { destroy_swapchain(); });

	// draw image size will match the window
	VkExtent3D draw_image_extent = {
		_window_extent.width,
		_window_extent.height,
		1,
	};

	// hardcoding the draw format to 32 bit float
	_draw_image.image_format = VK_FORMAT_R16G16B16A16_SFLOAT;
	_draw_image.image_extent = draw_image_extent;

	VkImageUsageFlags draw_image_uses = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	VkImageCreateInfo rimg_info = vkinit::image_create_info(
			_draw_image.image_format, draw_image_uses, draw_image_extent);

	// for the draw image, we want to allocate it from gpu local memory
	VmaAllocationCreateInfo rimg_alloc_info = {};
	rimg_alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	rimg_alloc_info.requiredFlags =
			VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	// allocate and create the image
	vmaCreateImage(_allocator, &rimg_info, &rimg_alloc_info, &_draw_image.image,
			&_draw_image.allocation, nullptr);

	// build a image-view for the draw image to use for rendering
	VkImageViewCreateInfo rview_info =
			vkinit::imageview_create_info(_draw_image.image_format,
					_draw_image.image, VK_IMAGE_ASPECT_COLOR_BIT);

	VK_CHECK(vkCreateImageView(
			_device, &rview_info, nullptr, &_draw_image.image_view));

	// create depth image
	_depth_image.image_format = VK_FORMAT_D32_SFLOAT;
	_depth_image.image_extent = draw_image_extent;

	VkImageUsageFlags depth_image_uses =
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	VkImageCreateInfo dimg_info = vkinit::image_create_info(
			_depth_image.image_format, depth_image_uses, draw_image_extent);

	//allocate and create the depth image
	vmaCreateImage(_allocator, &dimg_info, &rimg_alloc_info,
			&_depth_image.image, &_depth_image.allocation, nullptr);

	// build a image-view for the depth image to use for rendering
	VkImageViewCreateInfo dview_info =
			vkinit::imageview_create_info(_depth_image.image_format,
					_depth_image.image, VK_IMAGE_ASPECT_DEPTH_BIT);

	VK_CHECK(vkCreateImageView(
			_device, &dview_info, nullptr, &_depth_image.image_view));

	// deletion queues
	_deletion_queue.push_function([this]() {
		vkDestroyImageView(_device, _draw_image.image_view, nullptr);
		vmaDestroyImage(_allocator, _draw_image.image, _draw_image.allocation);

		vkDestroyImageView(_device, _depth_image.image_view, nullptr);
		vmaDestroyImage(
				_allocator, _depth_image.image, _depth_image.allocation);
	});
}

void VulkanEngine::init_commands() {
	//create a command pool for commands submitted to the graphics queue.
	//we also want the pool to allow for resetting of individual command buffers
	VkCommandPoolCreateInfo command_pool_info =
			vkinit::command_pool_create_info(_graphics_queue_family,
					VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		VK_CHECK(vkCreateCommandPool(_device, &command_pool_info, nullptr,
				&_frames[i].command_pool));
		// command pool cleanup
		_deletion_queue.push_function([this, i]() {
			vkDestroyCommandPool(_device, _frames[i].command_pool, nullptr);
		});

		VkCommandBufferAllocateInfo cmd_alloc_info =
				vkinit::command_buffer_allocate_info(
						_frames[i].command_pool, 1);

		VK_CHECK(vkAllocateCommandBuffers(
				_device, &cmd_alloc_info, &_frames[i].main_command_buffer));
	}

	// create imgui commands
	{
		VK_CHECK(vkCreateCommandPool(
				_device, &command_pool_info, nullptr, &_imm_command_pool));

		// allocate the command buffer for immediate submits
		VkCommandBufferAllocateInfo cmd_alloc_info =
				vkinit::command_buffer_allocate_info(_imm_command_pool, 1);

		VK_CHECK(vkAllocateCommandBuffers(
				_device, &cmd_alloc_info, &_imm_command_buffer));

		_deletion_queue.push_function([this]() {
			vkDestroyCommandPool(_device, _imm_command_pool, nullptr);
		});
	}
}

void VulkanEngine::init_sync_structures() {
	//create synchronization structures
	//one fence to control when the gpu has finished rendering the frame,
	//and 2 semaphores to synchronize rendering with swapchain
	VkFenceCreateInfo fence_info =
			vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
	VkSemaphoreCreateInfo semaphore_info = vkinit::semaphore_create_info();

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		// On the fence, we are using the flag VK_FENCE_CREATE_SIGNALED_BIT.
		// This is very important, as it allows us to wait on a freshly created
		// fence without causing errors. If we did not have that bit, when we
		// call into WaitFences the first frame, before the gpu is doing work,
		// the thread will be blocked.
		VK_CHECK(vkCreateFence(
				_device, &fence_info, nullptr, &_frames[i].render_fence));

		VK_CHECK(vkCreateSemaphore(_device, &semaphore_info, nullptr,
				&_frames[i].swapchain_semaphore));
		VK_CHECK(vkCreateSemaphore(_device, &semaphore_info, nullptr,
				&_frames[i].render_semaphore));

		// sync object cleanup
		_deletion_queue.push_function([this, i]() {
			vkDestroyFence(_device, _frames[i].render_fence, nullptr);
			vkDestroySemaphore(_device, _frames[i].render_semaphore, nullptr);
			vkDestroySemaphore(
					_device, _frames[i].swapchain_semaphore, nullptr);
		});
	}

	// imgui sync structures
	{
		VK_CHECK(vkCreateFence(_device, &fence_info, nullptr, &_imm_fence));
		_deletion_queue.push_function(
				[this]() { vkDestroyFence(_device, _imm_fence, nullptr); });
	}
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height) {
	vkb::SwapchainBuilder swapchain_builder{ _chosenGPU, _device, _surface };

	_swapchain_image_format = VK_FORMAT_B8G8R8A8_UNORM;

	vkb::Swapchain vkb_swapchain =
			swapchain_builder
					.set_desired_format(VkSurfaceFormatKHR{
							.format = _swapchain_image_format,
							.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
					.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
					.set_desired_extent(width, height)
					.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
					.build()
					.value();

	_swapchain_extent = vkb_swapchain.extent;

	_swapchain = vkb_swapchain.swapchain;
	_swapchain_images = vkb_swapchain.get_images().value();
	_swapchain_image_views = vkb_swapchain.get_image_views().value();
}

void VulkanEngine::destroy_swapchain() {
	vkDestroySwapchainKHR(_device, _swapchain, nullptr);

	// destroy swapchain resources
	for (int i = 0; i < _swapchain_image_views.size(); i++) {
		vkDestroyImageView(_device, _swapchain_image_views[i], nullptr);
	}
}

void VulkanEngine::resize_swapchain() {
	vkDeviceWaitIdle(_device);

	destroy_swapchain();

	int w, h;
	SDL_GetWindowSize(_window, &w, &h);

	_window_extent.width = w;
	_window_extent.height = h;

	create_swapchain(_window_extent.width, _window_extent.height);

	_resize_requested = false;
}

void VulkanEngine::init_descriptors() {
	// create a descriptor pool that will hold 10 sets with 1 image each
	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
	};

	_global_descriptor_allocator.init(_device, 10, sizes);

	// make the descriptor set layout for our compute draw
	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		_draw_image_descriptor_layout =
				builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
	}

	// allocate a descriptor set for our draw image
	_draw_image_descriptors = _global_descriptor_allocator.allocate(
			_device, _draw_image_descriptor_layout);

#if 0
	VkDescriptorImageInfo img_info = {
		.imageView = _draw_image.image_view,
		.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	VkWriteDescriptorSet draw_image_write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = nullptr,
		.dstSet = _draw_image_descriptors,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.pImageInfo = &img_info,
	};

    vkUpdateDescriptorSets(_device, 1, &draw_image_write, 0, nullptr);
#else
	DescriptorWriter writer;
	writer.write_image(0, _draw_image.image_view, VK_NULL_HANDLE,
			VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.update_set(_device, _draw_image_descriptors);
#endif

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		// create a descriptor pool
		std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frame_sizes = {
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 },
		};

		_frames[i].frame_descriptors = DescriptorAllocatorGrowable{};
		_frames[i].frame_descriptors.init(_device, 1000, frame_sizes);

		_deletion_queue.push_function([&, i]() {
			_frames[i].frame_descriptors.destroy_pools(_device);
		});
	}

	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		_gpu_scene_data_descriptor_layout = builder.build(_device,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	}

	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		_single_image_descriptor_layout =
				builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
}

void VulkanEngine::init_pipelines() {
	init_background_pipelines();
	init_mesh_pipeline();

	_metal_rough_material.build_pipeline(this);
}

void VulkanEngine::init_background_pipelines() {
	VkPushConstantRange push_constants = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset = 0,
		.size = sizeof(ComputePushConstants),
	};

	VkPipelineLayoutCreateInfo compute_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pNext = nullptr,
		.setLayoutCount = 1,
		.pSetLayouts = &_draw_image_descriptor_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constants,
	};

	VK_CHECK(vkCreatePipelineLayout(_device, &compute_layout_create_info,
			nullptr, &_background_pipeline_layout));

	// layout code
	VkShaderModule gradient_shader;
	if (!vkutil::load_shader_module(
				"gradient_color.comp.spv", _device, &gradient_shader)) {
		fmt::print("Error when building the compute shader!\n");
	}

	VkShaderModule sky_shader;
	if (!vkutil::load_shader_module("sky.comp.spv", _device, &sky_shader)) {
		fmt::print("Error when building the compute shader!\n");
	}

	VkPipelineShaderStageCreateInfo stage_info =
			vkinit::pipeline_shader_stage_create_info(
					VK_SHADER_STAGE_COMPUTE_BIT, gradient_shader);

	VkComputePipelineCreateInfo compute_pipeline_create_info = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.pNext = nullptr,
		.stage = stage_info,
		.layout = _background_pipeline_layout,
	};

	ComputeEffect gradient = {
		.name = "gradient",
		.layout = _background_pipeline_layout,
		.data = {
            //default colors
            .data1 = glm::vec4(1, 0, 0, 1),
            .data2 = glm::vec4(0, 0, 1, 1),
        },
	};

	VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
			&compute_pipeline_create_info, nullptr, &gradient.pipeline));

	// change the shader module only to create the sky shader
	compute_pipeline_create_info.stage.module = sky_shader;

	ComputeEffect sky = {
		.name = "sky",
		.layout = _background_pipeline_layout,
		.data = {
            // default sky parameters
            .data1 = glm::vec4(0.1, 0.2, 0.4, 0.97),
        },
	};

	VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
			&compute_pipeline_create_info, nullptr, &sky.pipeline));

	// add the 2 background effects into the array
	_background_effects.push_back(gradient);
	_background_effects.push_back(sky);

	vkDestroyShaderModule(_device, gradient_shader, nullptr);
	vkDestroyShaderModule(_device, sky_shader, nullptr);

	_deletion_queue.push_function([this, gradient, sky]() {
		vkDestroyPipelineLayout(_device, _background_pipeline_layout, nullptr);
		vkDestroyPipeline(_device, gradient.pipeline, nullptr);
		vkDestroyPipeline(_device, sky.pipeline, nullptr);
	});
}

void VulkanEngine::init_mesh_pipeline() {
	VkShaderModule triangle_frag_shader;
	if (!vkutil::load_shader_module(
				"tex_image.frag.spv", _device, &triangle_frag_shader)) {
		fmt::print("Error when building the mesh fragment shader!\n");
	} else {
		fmt::print("Mesh fragment shader successfully loaded\n");
	}

	VkShaderModule triangle_vert_shader;
	if (!vkutil::load_shader_module(
				"colored_mesh.vert.spv", _device, &triangle_vert_shader)) {
		fmt::print("Error when building the mesh vertex shader!\n");
	} else {
		fmt::print("Mesh vertex shader successfully loaded\n");
	}

	VkPushConstantRange buffer_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = sizeof(GPUDrawPushConstants),
	};

	// build the pipeline layout that controls the inputs/outputs of the shader
	VkPipelineLayoutCreateInfo pipeline_layout_info =
			vkinit::pipeline_layout_create_info();
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges = &buffer_range;
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts = &_single_image_descriptor_layout;

	VK_CHECK(vkCreatePipelineLayout(
			_device, &pipeline_layout_info, nullptr, &_mesh_pipeline_layout));

	PipelineBuilder pipeline_builder;
	pipeline_builder.pipeline_layout = _mesh_pipeline_layout;
	pipeline_builder.set_shaders(triangle_vert_shader, triangle_frag_shader);
	pipeline_builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipeline_builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipeline_builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipeline_builder.set_multisampling_none();
	pipeline_builder.disable_blending();
	pipeline_builder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
	pipeline_builder.set_color_attachment_format(_draw_image.image_format);
	pipeline_builder.set_depth_format(_depth_image.image_format);

	// finally build the pipeline
	_mesh_pipeline = pipeline_builder.build_pipeline(_device);

	// clean structures
	vkDestroyShaderModule(_device, triangle_frag_shader, nullptr);
	vkDestroyShaderModule(_device, triangle_vert_shader, nullptr);

	_deletion_queue.push_function([this]() {
		vkDestroyPipelineLayout(_device, _mesh_pipeline_layout, nullptr);
		vkDestroyPipeline(_device, _mesh_pipeline, nullptr);
	});
}

AllocatedBuffer VulkanEngine::create_buffer(size_t alloc_size,
		VkBufferUsageFlags usage, VmaMemoryUsage memory_usage) {
	// allocate buffer
	VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = nullptr,
		.size = alloc_size,
		.usage = usage,
	};

	VmaAllocationCreateInfo vma_alloc_info = {
		.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
		.usage = memory_usage,
	};

	// allocate the buffer
	AllocatedBuffer new_buffer;
	VK_CHECK(vmaCreateBuffer(_allocator, &buffer_info, &vma_alloc_info,
			&new_buffer.buffer, &new_buffer.allocation, &new_buffer.info));

	return new_buffer;
}

void VulkanEngine::destroy_buffer(const AllocatedBuffer& buffer) {
	vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
}

void VulkanEngine::init_imgui() {
	// 1: create descriptor pool for IMGUI
	//  the size of the pool is very oversize, but it's copied from imgui demo
	//  itself.
	VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 1000;
	pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
	pool_info.pPoolSizes = pool_sizes;

	VkDescriptorPool imgui_pool;
	VK_CHECK(vkCreateDescriptorPool(_device, &pool_info, nullptr, &imgui_pool));

	// initialize imgui
	ImGui::CreateContext();

	// initialize imgui for sdl2
	ImGui_ImplSDL2_InitForVulkan(_window);

	// initialize imgui for vulkan
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.Instance = _instance;
	init_info.PhysicalDevice = _chosenGPU;
	init_info.Device = _device;
	init_info.Queue = _graphics_queue;
	init_info.DescriptorPool = imgui_pool;
	init_info.MinImageCount = 3;
	init_info.ImageCount = 3;
	init_info.UseDynamicRendering = true;

	// dynamic rendering parameters for imgui to use
	init_info.PipelineRenderingCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO
	};
	init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats =
			&_swapchain_image_format;

	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init(&init_info);

	ImGui_ImplVulkan_CreateFontsTexture();

	// add the destroy the imgui created structures
	_deletion_queue.push_function([this, imgui_pool]() {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool(_device, imgui_pool, nullptr);
	});
}

void VulkanEngine::draw_imgui(
		VkCommandBuffer cmd, VkImageView target_image_view) {
	VkRenderingAttachmentInfo color_attachment =
			vkinit::attachment_info(target_image_view, nullptr,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo render_info = vkinit::rendering_info(
			_swapchain_extent, &color_attachment, nullptr);

	vkCmdBeginRendering(cmd, &render_info);

	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

	vkCmdEndRendering(cmd);
}

void VulkanEngine::immediate_submit(
		std::function<void(VkCommandBuffer cmd)>&& function) {
	VK_CHECK(vkResetFences(_device, 1, &_imm_fence));
	VK_CHECK(vkResetCommandBuffer(_imm_command_buffer, 0));

	VkCommandBuffer cmd = _imm_command_buffer;

	VkCommandBufferBeginInfo cmd_begin_info = vkinit::command_buffer_begin_info(
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	VK_CHECK(vkBeginCommandBuffer(_imm_command_buffer, &cmd_begin_info));

	function(cmd);

	VK_CHECK(vkEndCommandBuffer(cmd));

	VkCommandBufferSubmitInfo cmd_info =
			vkinit::command_buffer_submit_info(cmd);
	VkSubmitInfo2 submit = vkinit::submit_info(&cmd_info, nullptr, nullptr);

	// submit command buffer to the queue and execute it.
	// _imm_fence will now block until the graphic commands finish execution
	VK_CHECK(vkQueueSubmit2(_graphics_queue, 1, &submit, _imm_fence));

	VK_CHECK(vkWaitForFences(_device, 1, &_imm_fence, true, UINT64_MAX));
}
