GBDK     = /c/gbdk
LCC      = $(GBDK)/bin/lcc

# CGB-only mode, aggressive SDCC optimisation
CFLAGS   = -msm83:gb \
           -Wl-yC \
           -Wf--max-allocs-per-node50000

BUILD    = build
TARGET   = $(BUILD)/odyssey.gbc

SRC      = src/main.c
HEADERS  = include/bg_tiles.h include/lut.h

# ------------------------------------------------------------------
all: assets $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(LCC) $(CFLAGS) -o $@ $(SRC)

assets: $(HEADERS)

$(HEADERS) &: tools/convert_assets.py assets/bg.png
	python tools/convert_assets.py

clean:
	rm -rf $(BUILD) include/bg_tiles.h include/lut.h

run: all
	@echo "ROM built: $(TARGET)"
	@echo "Open in a GBC emulator (BGB, Emulicious, SameBoy, etc.)"

.PHONY: all assets clean run
