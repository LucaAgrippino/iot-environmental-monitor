IMAGE := lcd-sim-builder:latest
BUILD_DIR := build
WORK := /work

.PHONY: image build run shell clean native shots

image:
	docker build -t $(IMAGE) .

build: image
	docker run --rm -v "$(CURDIR)/../..:$(WORK)/repo" -w $(WORK)/repo/simulator/lcd-ui \
	    $(IMAGE) bash -c "cmake -S . -B $(BUILD_DIR) -G Ninja && cmake --build $(BUILD_DIR)"

# Build with the host's own SDL2, CMake and Ninja (no Docker): output in build-native/.
native:
	cmake -S . -B build-native -G Ninja && cmake --build build-native

# Render every screen and state of the design pack to PPM files in shots/ (needs `make native`).
shots: native
	./shots.sh build-native/lcd_sim shots

run:
	./$(BUILD_DIR)/lcd_sim

shell: image
	docker run --rm -it -v "$(CURDIR)/../..:$(WORK)/repo" -w $(WORK)/repo/simulator/lcd-ui \
	    $(IMAGE) bash

clean:
	rm -rf $(BUILD_DIR) build-native shots
