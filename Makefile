.DEFAULT_GOAL := help
.PHONY: help all build build-all check play shcd shcd-reference shk1-rom \
        prepare-roms shbs-rom shbs-rom-if-available \
        shcd-if-available shk1-rom-if-available sh-compilation \
        sh-compilation-if-available derived-all require-title \
        launcher-macos launcher-linux launcher-windows package-macos package-linux package-windows

TITLE ?=
DISC ?=
SONICCD_DISC ?= $(ROM_DIR)/cd/shcd/shcd.cue
COMPILATION_DISC ?=
MENU_IMAGE ?=
SONIC_COMPILATION_OUT ?= build/ultimate-sh
ROM_DIR ?= ../roms
CORES ?= 4
VERSION ?=

ifeq ($(strip $(PS_SCD_BIOS)),)
  ifeq ($(shell uname -s),Darwin)
    SCD_BIOS_DIR := $(HOME)/Library/Application Support/PixelSorcery/bios
  else ifneq ($(strip $(MSYSTEM)),)
    SCD_BIOS_DIR := $(APPDATA)/PixelSorcery/bios
  else
    SCD_BIOS_DIR := $(or $(XDG_DATA_HOME),$(HOME)/.local/share)/pixelsorcery/bios
  endif
else
  SCD_BIOS_DIR := $(PS_SCD_BIOS)
endif

help:
	@printf '%s\n' \
	  'make all                                      Build game ports plus local ROM/disc outputs when inputs exist' \
	  'make build TITLE=shk1                       Build one native port (title code or folder)' \
	  'make check TITLE=shk1                       Build and run one title self-check' \
	  'make play TITLE=shk1                        Build if needed, then play' \
	  'make shk1-rom                               Build the Knuckles-in-Sonic-1 Genesis ROM when source ROMs exist' \
	  'make prepare-roms                          Find dumps and create available lock-on/derived ROMs' \
	  'make shbs-rom                       Build the SHBS Genesis lock-on ROM' \
	  'make derived-all                          Build all locally available ROM/disc-derived outputs' \
	  'make shcd DISC=/path/to/disc.cue         Build native SHCD (requires disc and BIOS)' \
	  'make shcd-reference DISC=...             Build SHCD reference and native executables' \
	  'make sh-compilation COMPILATION_DISC=... MENU_IMAGE=...  Build the Ultimate SH Mega CD disc' \
	  'make launcher-macos                         Package the SH Collection as a macOS app' \
	  'make launcher-linux                         Build/open the Linux SH Collection shelf' \
	  'make launcher-windows                       Build/open the Windows shelf (MSYS2)' \
	  'make package-macos                         Build the PixelSorcery macOS DMG' \
	  'make package-linux                         Build the PixelSorcery Linux DEB' \
	  'make package-windows                       Build the PixelSorcery Windows ZIP/EXE (MSYS2)' \
	  'Inputs for all: ROM_DIR (default ../roms), COMPILATION_DISC, MENU_IMAGE; SHCD cue is auto-found in ROM_DIR'

# Aggregate proxy: the native ports plus every derived build whose required local inputs exist.
all:
	$(MAKE) prepare-roms ROM_DIR="$(ROM_DIR)"
	$(MAKE) build-all CORES=$(CORES)
	$(MAKE) derived-all CORES=$(CORES) ROM_DIR="$(ROM_DIR)" SONICCD_DISC="$(SONICCD_DISC)" COMPILATION_DISC="$(COMPILATION_DISC)" MENU_IMAGE="$(MENU_IMAGE)" SONIC_COMPILATION_OUT="$(SONIC_COMPILATION_OUT)"

build: require-title
	./engine/tools/build_all.sh -j $(CORES) $(TITLE)

build-all:
	./engine/tools/build_all.sh -j $(CORES)

check: require-title
	./engine/tools/build_all.sh -j $(CORES) --check $(TITLE)

play: require-title
	./play.sh $(TITLE)

require-title:
	@test -n "$(TITLE)" || { echo 'Set TITLE (for example: make build TITLE=shk1)' >&2; exit 2; }

# Native/reference SHCD executables from a player's disc and BIOS.
shcd:
	@test -n "$(DISC)" || { echo 'Set DISC to your SHCD .cue file' >&2; exit 2; }
	./games/shcd/tools/generate.sh "$(DISC)"
	cmake -S games/shcd -B games/shcd/build -G Ninja -DCMAKE_BUILD_TYPE=Release
	ninja -C games/shcd/build -j $(CORES) shcd_native

shcd-reference:
	@test -n "$(DISC)" || { echo 'Set DISC to your SHCD .cue file' >&2; exit 2; }
	./games/shcd/tools/generate.sh "$(DISC)"
	cmake -S games/shcd -B games/shcd/build -G Ninja -DCMAKE_BUILD_TYPE=Release
	ninja -C games/shcd/build -j $(CORES)

# Find owned dumps, link recognized inputs into ../roms, and construct the lock-on titles from them.
prepare-roms:
	engine/.venv/bin/python engine/tools/build_ui.py --prepare "$(ROM_DIR)"

# SHBS is the SHK + SH1 lock-on; it is a launcher title, not a separate recompilation.
shbs-rom:
	@test -f "$(ROM_DIR)/shk.bin" -a -f "$(ROM_DIR)/sh1.bin" || { echo "Need $(ROM_DIR)/shk.bin and sh1.bin" >&2; exit 1; }
	engine/.venv/bin/python engine/tools/lockon.py "$(ROM_DIR)/shk.bin" "$(ROM_DIR)/sh1.bin" "$(ROM_DIR)/shk_sh1.bin"

shbs-rom-if-available:
	@if [ -f "$(ROM_DIR)/shk_sh1.bin" ]; then \
	  echo "SHBS ROM already present: $(ROM_DIR)/shk_sh1.bin"; \
	elif [ -f "$(ROM_DIR)/shk.bin" ] && [ -f "$(ROM_DIR)/sh1.bin" ]; then \
	  $(MAKE) --no-print-directory shbs-rom ROM_DIR="$(ROM_DIR)"; \
	else echo "SKIP SHBS lock-on ROM: needs $(ROM_DIR)/shk.bin + sh1.bin"; fi

# Genesis cartridge image derived from the player's SH1 REV00 and SHK2 dumps.
shk1-rom:
	./games/shk1/tools/ensure_rom.sh "$(ROM_DIR)"
	@test -f "$(ROM_DIR)/shk1.bin" || { echo "Need $(ROM_DIR)/sh1.bin and shk_sh2.bin" >&2; exit 1; }
	@echo "Genesis ROM: $(ROM_DIR)/shk1.bin"

shk1-rom-if-available:
	@if [ -f "$(ROM_DIR)/sh1.bin" ] && [ -f "$(ROM_DIR)/shk_sh2.bin" ]; then \
	  $(MAKE) --no-print-directory shk1-rom ROM_DIR="$(ROM_DIR)"; \
	else echo "SKIP SHK1 Genesis ROM: needs $(ROM_DIR)/sh1.bin + shk_sh2.bin"; fi

# Ultimate SH: a bootable Mega CD compilation built from the owner's SHCD disc, menu art and ROMs.
sh-compilation:
	@test -n "$(COMPILATION_DISC)" -a -f "$(COMPILATION_DISC)" || { echo 'Set COMPILATION_DISC to a SHCD (Europe) .cue file' >&2; exit 2; }
	@test -n "$(MENU_IMAGE)" -a -f "$(MENU_IMAGE)" || { echo 'Set MENU_IMAGE to the 4x2 game-select picture (.png or converted .bin)' >&2; exit 2; }
	@test -f "$(ROM_DIR)/sh1.bin" || { echo "Need $(ROM_DIR)/sh1.bin" >&2; exit 1; }
	ROM="$(ROM_DIR)/sh1.bin" ROM2="$(ROM_DIR)/sh2.bin" ROM3="$(ROM_DIR)/sh3.bin" \
	  ROM4="$(ROM_DIR)/shk.bin" ROM5="$(ROM_DIR)/shk_sh1.bin" ROM7="$(ROM_DIR)/sh3k.bin" \
	  ./engine/tools/megacd/ultimate_sh.sh "$(COMPILATION_DISC)" "$(MENU_IMAGE)" "$(SONIC_COMPILATION_OUT)"

sh-compilation-if-available:
	@if [ -n "$(COMPILATION_DISC)" ] && [ -f "$(COMPILATION_DISC)" ] && \
	   [ -n "$(MENU_IMAGE)" ] && [ -f "$(MENU_IMAGE)" ] && [ -f "$(ROM_DIR)/sh1.bin" ]; then \
	  $(MAKE) --no-print-directory sh-compilation ROM_DIR="$(ROM_DIR)" COMPILATION_DISC="$(COMPILATION_DISC)" MENU_IMAGE="$(MENU_IMAGE)" SONIC_COMPILATION_OUT="$(SONIC_COMPILATION_OUT)"; \
	else echo 'SKIP Ultimate SH Mega CD disc: set COMPILATION_DISC to your Europe cue and MENU_IMAGE, and provide ROM_DIR/sh1.bin'; fi

shcd-if-available:
	@if [ -n "$(SONICCD_DISC)" ] && [ -f "$(SONICCD_DISC)" ] && \
	   ls "$(SCD_BIOS_DIR)"/scd_*.bin >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory shcd DISC="$(SONICCD_DISC)" CORES=$(CORES); \
	else echo 'SKIP SHCD native executable: set SONICCD_DISC and provide the matching BIOS in PS_SCD_BIOS (or the default BIOS folder)'; fi

derived-all: shk1-rom-if-available shbs-rom-if-available sh-compilation-if-available shcd-if-available

launcher-macos:
	./engine/tools/make_launcher.sh launchers/sh.txt

launcher-linux:
	./engine/tools/linux/make_shelf.sh sh

launcher-windows:
	./engine/tools/windows/make_shelf.sh sh

package-macos:
	./engine/tools/macapp/make_dmg.sh $(if $(VERSION),--version $(VERSION))

package-linux:
	./engine/tools/linux/make_deb.sh $(if $(VERSION),--version $(VERSION))

package-windows:
	./engine/tools/windows/make_zip.sh $(if $(VERSION),--version $(VERSION))
