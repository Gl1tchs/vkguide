CMAKE = cmake
CMAKE_FLAGS = -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DCMAKE_BUILD_TYPE=Debug

BUILD_PATH = build/

all: configure build
	
configure:
	@echo "Configuring cmake..."
	$(CMAKE) -S . -B $(BUILD_PATH) $(CMAKE_FLAGS)
	
build: compile_shaders
	@echo "Building engine..."
	$(CMAKE) --build $(BUILD_PATH)

compile_shaders: utils
	@echo "Compiling shaders..."
	$(MAKE) -C shaders -f Makefile.mk

utils:
	@echo "Building utilities..."
	$(MAKE) -C utils -f Makefile.mk

clean:
	@rm -rf $(BUILD_PATH)

.PHONY: all clean utils
