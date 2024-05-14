GLSLC = glslc
GLSL_FLAGS = -I include --target-env=vulkan1.3 -std=450 -O

SRC_DIR = src
BUILD_DIR = ../build/shaders

VS_FILES := $(wildcard $(SRC_DIR)/*.vert)
FS_FILES := $(wildcard $(SRC_DIR)/*.frag)
COMP_FILES := $(wildcard $(SRC_DIR)/*.comp)

SPV_VS := $(patsubst $(SRC_DIR)/%.vert,$(BUILD_DIR)/%.vert.spv,$(VS_FILES))
SPV_FS := $(patsubst $(SRC_DIR)/%.frag,$(BUILD_DIR)/%.frag.spv,$(FS_FILES))
SPV_COMP := $(patsubst $(SRC_DIR)/%.comp,$(BUILD_DIR)/%.comp.spv,$(COMP_FILES))

all: $(SPV_VS) $(SPV_FS) $(SPV_COMP) bundle_shaders

$(BUILD_DIR)/%.vert.spv: $(SRC_DIR)/%.vert
	@mkdir -p $(BUILD_DIR)
	$(GLSLC) $(GLSL_FLAGS) -o $@ $<

$(BUILD_DIR)/%.frag.spv: $(SRC_DIR)/%.frag
	@mkdir -p $(BUILD_DIR)
	$(GLSLC) $(GLSL_FLAGS) -o $@ $<

$(BUILD_DIR)/%.comp.spv: $(SRC_DIR)/%.comp
	@mkdir -p $(BUILD_DIR)
	$(GLSLC) $(GLSL_FLAGS) -o $@ $<

bundle_shaders:
	@mkdir -p ../build/include
	../build/utils/bundler ../build/include/shader_bundle.gen.h $(SPV_VS) $(SPV_FS) $(SPV_COMP)

clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf ../build/include

.PHONY: all clean
