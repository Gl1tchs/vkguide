#pragma once

#include <vulkan/vulkan.h>

namespace vkutil {

void transition_image(VkCommandBuffer cmd, VkImage image,
		VkImageLayout current_layout, VkImageLayout new_layout);

void copy_image_to_image(VkCommandBuffer cmd, VkImage source,
		VkImage destination, VkExtent2D src_size, VkExtent2D dst_size);
 
}
