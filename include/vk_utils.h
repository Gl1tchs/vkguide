#pragma once

#include "vk_types.h"

namespace vkutil {

glm::mat4 make_inf_reversed_z_proj_rh(
		float fov_y_radians, float aspect_wby_h, float z_near);

}
