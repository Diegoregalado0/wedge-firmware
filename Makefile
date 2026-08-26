# Host build of the simulator. The device build lives under platform/esp32s3
# and is driven by idf.py; both compile the same core/ sources.

CC ?= cc
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)

CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
          -Icore/include -Icore/src -Icore/src/faces $(SDL_CFLAGS)
LDFLAGS := $(SDL_LIBS) -lm

CORE := $(wildcard core/src/*.c) $(wildcard core/src/faces/*.c) $(wildcard core/src/fonts/*.c)
SIM := platform/sdl/main.c

BUILD := build
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(CORE) $(SIM))

all: $(BUILD)/wedge-sim

$(BUILD)/wedge-sim: $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

fonts:
	python3 tools/bake_fonts.py

run: all
	./$(BUILD)/wedge-sim

clean:
	rm -rf $(BUILD)

.PHONY: all fonts run clean

# Headless frame renderer, for reviewing composition without a window.
CORE_ONLY_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Icore/include -Icore/src -Icore/src/faces
shots: $(CORE)
	@mkdir -p $(BUILD)/shots
	$(CC) $(CORE_ONLY_CFLAGS) $(CORE) tools/render_frames.c -o $(BUILD)/render-frames -lm
	./$(BUILD)/render-frames $(BUILD)/shots
	@python3 tools/ppm_to_png.py $(BUILD)/shots

.PHONY: shots

# Regression test for the celestial path. The moon's arc was two expressions
# meeting at 06:00 that disagreed by a third of the screen; this fails if that
# ever comes back.
test-moon: $(CORE)
	@mkdir -p $(BUILD)
	$(CC) $(CORE_ONLY_CFLAGS) core/src/canvas.c core/src/scene.c core/src/spring.c \
		tools/test_moon_path.c -o $(BUILD)/test-moon -lm
	./$(BUILD)/test-moon

.PHONY: test-moon

# The web build. Two outputs from the same core: one for a browser (the
# artifact) and one for node (the test harness). It was hand-run before, which
# meant the tests could pass against a bundle older than the code they claimed
# to cover.
EMSDK ?= $(HOME)/emsdk
WASM_SRC := $(CORE) platform/wasm/main.c
WASM_FLAGS := -O2 -Icore/include -Icore/src -Icore/src/faces \
              -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","HEAPU8","HEAPU32"]' \
              -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 -s EXPORT_ES6=1

wasm:
	@. $(EMSDK)/emsdk_env.sh >/dev/null 2>&1 && \
	  emcc $(WASM_SRC) $(WASM_FLAGS) -s ENVIRONMENT=web -s SINGLE_FILE=1 \
	       -o $(BUILD)/wasm/wedge.js && \
	  emcc $(WASM_SRC) $(WASM_FLAGS) -s ENVIRONMENT=node -s SINGLE_FILE=1 \
	       -o $(BUILD)/wasm/wedge-node.js

# Pacific daylight saving, by rule. The transitions move every year, so this
# checks both sides of both boundaries in two different years rather than
# trusting that the arithmetic looked right.
test-tz: $(CORE)
	@mkdir -p $(BUILD)
	$(CC) $(CORE_ONLY_CFLAGS) $(CORE) tools/test_timezone.c -o $(BUILD)/test-tz -lm
	./$(BUILD)/test-tz

test: wasm test-moon test-tz
	cd $(BUILD)/wasm && node test.mjs && node bank.mjs

.PHONY: fonts run clean shots wasm test test-tz
