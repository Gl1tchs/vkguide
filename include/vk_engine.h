#pragma once

#include "vk_descriptors.h"
#include "vk_types.h"

struct DeletionQueue {
	std::deque<std::function<void()>> deletors;

	inline void push_function(std::function<void()>&& func) {
		deletors.push_back(func);
	}

	inline void flush() {
		for (auto it = deletors.rbegin(); it != deletors.rend(); it++) {
			(*it)(); // call function
		}

		deletors.clear();
	}
};

struct FrameData {
	VkCommandPool command_pool;
	VkCommandBuffer main_command_buffer;

	VkSemaphore swapchain_semaphore, render_semaphore;
	VkFence render_fence;
};

constexpr unsigned int FRAME_OVERLAP = 2;

struct ComputePushConstants {
	glm::vec4 data1;
	glm::vec4 data2;
	glm::vec4 data3;
	glm::vec4 data4;
};

struct ComputeEffect {
	const char* name;

	VkPipeline pipeline;
	VkPipelineLayout layout;

	ComputePushConstants data;
};

class VulkanEngine {
public:
	static VulkanEngine& get();

	// initializes everything in the engine
	void init();

	// shuts down the engine
	void cleanup();

	// run main loop
	void run();

private:
	void draw();

	void draw_geometry(VkCommandBuffer cmd);

	void draw_background(VkCommandBuffer cmd);

	void init_vulkan();

	void init_swapchain();

	void init_commands();

	void init_sync_structures();

	void create_swapchain(uint32_t width, uint32_t height);

	void destroy_swapchain();

	void init_descriptors();

	void init_pipelines();

	void init_background_pipelines();

	void init_triangle_pipeline();

	FrameData& get_current_frame() {
		return _frames[_frame_number % FRAME_OVERLAP];
	};

private:
	bool _is_initialized{ false };
	int _frame_number{ 0 };
	bool _stop_rendering{ false };
	VkExtent2D _window_extent{ 1700, 900 };

	struct SDL_Window* _window{ nullptr };

	DeletionQueue _deletion_queue;

	VkInstance _instance;
	VkDebugUtilsMessengerEXT _debug_messenger;
	VkPhysicalDevice _chosenGPU;
	VkDevice _device;
	VkSurfaceKHR _surface;
	VmaAllocator _allocator;

	VkSwapchainKHR _swapchain;
	VkFormat _swapchain_image_format;

	// draw resources
	AllocatedImage _draw_image;
	VkExtent2D _draw_extent;

	std::vector<VkImage> _swapchain_images;
	std::vector<VkImageView> _swapchain_image_views;
	VkExtent2D _swapchain_extent;

	FrameData _frames[FRAME_OVERLAP];
	VkQueue _graphics_queue;
	uint32_t _graphics_queue_family;

	DescriptorAllocator _global_descriptor_allocator;

	VkDescriptorSet _draw_image_descriptors;
	VkDescriptorSetLayout _draw_image_descriptor_layout;

	// VkPipeline _gradient_pipeline;
	VkPipelineLayout _background_pipeline_layout;

	std::vector<ComputeEffect> _background_effects;
	int _current_background_effect{ 0 };

	VkPipelineLayout _triangle_pipeline_layout;
	VkPipeline _triangle_pipeline;

private:
	// imgui stuff
	VkFence _imm_fence;
	VkCommandBuffer _imm_command_buffer;
	VkCommandPool _imm_command_pool;

	void init_imgui();

	void draw_imgui(VkCommandBuffer cmd, VkImageView target_image_view);

	void immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function);
};
