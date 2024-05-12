#include "vk_engine.h"

#include "vk_descriptors.h"
#include "vk_images.h"
#include "vk_initializers.h"
#include "vk_pipelines.h"
#include "vk_types.h"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vulkan/vulkan_core.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <chrono>
#include <thread>

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

	SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);

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

	// everything went fine
	_is_initialized = true;
}

void VulkanEngine::cleanup() {
	if (_is_initialized) {
		//make sure the gpu has stopped doing its things
		vkDeviceWaitIdle(_device);

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

		// imgui new frame
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();

		if (ImGui::Begin("background")) {
			ComputeEffect& selected =
					_background_effects[_current_background_effect];

			ImGui::Text("Selected effect: %s", selected.name);

			ImGui::SliderInt("Effect Index", &_current_background_effect, 0,
					_background_effects.size() - 1);

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

void VulkanEngine::draw() {
	// wait until the gpu has finished rendering the last frame. Timeout of 1
	// second
	VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame().render_fence,
			true, ONE_SECOND_IN_NANOSECONDS));
	VK_CHECK(vkResetFences(_device, 1, &get_current_frame().render_fence));

	// request image from the swapchain
	uint32_t swapchain_image_index;
	VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain,
			ONE_SECOND_IN_NANOSECONDS, get_current_frame().swapchain_semaphore,
			nullptr, &swapchain_image_index));

	// naming it cmd for shorter writing
	VkCommandBuffer cmd = get_current_frame().main_command_buffer;

	// now that we are sure that the commands finished executing, we can safely
	// reset the command buffer to begin recording again.
	VK_CHECK(vkResetCommandBuffer(cmd, 0));

	// begin the command buffer recording. We will use this command buffer
	// exactly once, so we want to let vulkan know that
	VkCommandBufferBeginInfo cmd_begin_info = vkinit::command_buffer_begin_info(
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	_draw_extent.width = _draw_image.image_extent.width;
	_draw_extent.height = _draw_image.image_extent.height;

	// start the command buffer recording
	VK_CHECK(vkBeginCommandBuffer(cmd, &cmd_begin_info));
	{
		// make the swapchain image into writeable mode before rendering
		vkutil::transition_image(cmd, _draw_image.image,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		// actual drawing code
		{
			// clear color
			draw_background(cmd);
		}

		// transition the draw image and the swapchain image into their correct
		// transfer layouts
		vkutil::transition_image(cmd, _draw_image.image,
				VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
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

	VK_CHECK(vkQueuePresentKHR(_graphics_queue, &present_info));

	// increase the number of frames drawn
	_frame_number++;
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
			_gradient_pipeline_layout, 0, 1, &_draw_image_descriptors, 0,
			nullptr);

	vkCmdPushConstants(cmd, _gradient_pipeline_layout,
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

	// deletion queues
	_deletion_queue.push_function([this]() {
		vkDestroyImageView(_device, _draw_image.image_view, nullptr);
		vmaDestroyImage(_allocator, _draw_image.image, _draw_image.allocation);
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
	//create syncronization structures
	//one fence to control when the gpu has finished rendering the frame,
	//and 2 semaphores to syncronize rendering with swapchain
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

void VulkanEngine::init_descriptors() {
	// create a descriptor pool that will hold 10 sets with 1 image each
	std::vector<DescriptorAllocator::PoolSizeRatio> sizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
	};

	_global_descriptor_allocator.init_pool(_device, 10, sizes);

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

	VkDescriptorImageInfo img_info{};
	img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	img_info.imageView = _draw_image.image_view;

	VkWriteDescriptorSet draw_image_write = {};
	draw_image_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	draw_image_write.pNext = nullptr;

	draw_image_write.dstBinding = 0;
	draw_image_write.dstSet = _draw_image_descriptors;
	draw_image_write.descriptorCount = 1;
	draw_image_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	draw_image_write.pImageInfo = &img_info;

	vkUpdateDescriptorSets(_device, 1, &draw_image_write, 0, nullptr);
}

void VulkanEngine::init_pipelines() { init_background_pipelines(); }

void VulkanEngine::init_background_pipelines() {
	VkPipelineLayoutCreateInfo compute_layout_create_info{};
	compute_layout_create_info.sType =
			VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	compute_layout_create_info.pNext = nullptr;
	compute_layout_create_info.pSetLayouts = &_draw_image_descriptor_layout;
	compute_layout_create_info.setLayoutCount = 1;

	VkPushConstantRange push_constants{};
	push_constants.offset = 0;
	push_constants.size = sizeof(ComputePushConstants);
	push_constants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	compute_layout_create_info.pushConstantRangeCount = 1;
	compute_layout_create_info.pPushConstantRanges = &push_constants;

	VK_CHECK(vkCreatePipelineLayout(_device, &compute_layout_create_info,
			nullptr, &_gradient_pipeline_layout));

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

	VkPipelineShaderStageCreateInfo stage_info{};
	stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage_info.pNext = nullptr;
	stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage_info.module = gradient_shader;
	stage_info.pName = "main";

	VkComputePipelineCreateInfo compute_pipeline_create_info = {};
	compute_pipeline_create_info.sType =
			VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	compute_pipeline_create_info.pNext = nullptr;
	compute_pipeline_create_info.layout = _gradient_pipeline_layout;
	compute_pipeline_create_info.stage = stage_info;

	ComputeEffect gradient;
	gradient.layout = _gradient_pipeline_layout;
	gradient.name = "gradient";
	gradient.data = {};

	//default colors
	gradient.data.data1 = glm::vec4(1, 0, 0, 1);
	gradient.data.data2 = glm::vec4(0, 0, 1, 1);

	VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
			&compute_pipeline_create_info, nullptr, &gradient.pipeline));

	// change the shader module only to create the sky shader
	compute_pipeline_create_info.stage.module = sky_shader;

	ComputeEffect sky;
	sky.layout = _gradient_pipeline_layout;
	sky.name = "sky";
	sky.data = {};
	//default sky parameters
	sky.data.data1 = glm::vec4(0.1, 0.2, 0.4, 0.97);

	VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
			&compute_pipeline_create_info, nullptr, &sky.pipeline));

	// add the 2 background effects into the array
	_background_effects.push_back(gradient);
	_background_effects.push_back(sky);

	vkDestroyShaderModule(_device, gradient_shader, nullptr);
	vkDestroyShaderModule(_device, sky_shader, nullptr);

	_deletion_queue.push_function([this, gradient, sky]() {
		vkDestroyPipelineLayout(_device, _gradient_pipeline_layout, nullptr);
		vkDestroyPipeline(_device, gradient.pipeline, nullptr);
		vkDestroyPipeline(_device, sky.pipeline, nullptr);
	});
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
