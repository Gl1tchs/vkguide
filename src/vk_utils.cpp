#include "vk_utils.h"

glm::mat4 vkutil::make_inf_reversed_z_proj_rh(
		float fov_y_radians, float aspect_wby_h, float z_near) {
	float f = 1.0f / tan(fov_y_radians / 2.0f);
	return glm::mat4(f / aspect_wby_h, 0.0f, 0.0f, 0.0f, 0.0f, f, 0.0f, 0.0f,
			0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, z_near, 0.0f);
}
