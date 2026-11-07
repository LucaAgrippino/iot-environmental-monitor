IMAGE := lcd-sim-builder:latest
BUILD_DIR := build
WORK := /work

.PHONY: image build run shell clean

image:
	docker build -t $(IMAGE) .

build: image
	docker run --rm -v "$(CURDIR)/../..:$(WORK)/repo" -w $(WORK)/repo/simulator/lcd-ui \
	    $(IMAGE) bash -c "cmake -S . -B $(BUILD_DIR) -G Ninja && cmake --build $(BUILD_DIR)"

run:
	./$(BUILD_DIR)/lcd_sim

shell: image
	docker run --rm -it -v "$(CURDIR)/../..:$(WORK)/repo" -w $(WORK)/repo/simulator/lcd-ui \
	    $(IMAGE) bash

clean:
	rm -rf $(BUILD_DIR)
