.DEFAULT_GOAL := help
.PHONY: help all build build-all check play shcd shcd-reference shk1-rom \
        prepare-roms shbs-rom shbs-rom-if-available \
        shcd-if-available shk1-rom-if-available sh-compilation \
        sh-compilation-if-available sh-compilation-all derived-all require-title \
        launcher-macos launcher-linux launcher-windows package-macos package-linux package-windows setup

TITLE ?=
DISC ?=
SONICCD_DISC ?= $(ROMS)/cd/shcd/shcd.cue
# Found when not given: the Europe SHCD disc (any .cue under ROM_DIR or ROMS, by its hash), and menu.png or menu.bin
# (else "-": ultimate_sh.sh draws a plain 4x2 card menu).
# COMPILATION_REGION: the SHCD disc the compilation is built on - US (default): 60 Hz, as the cartridges;
# EU: 50 Hz (every game runs and plays its music slower); JP: 60 Hz, not working yet (the Japanese BIOS stops
# SH2's load partway). Its SHCD card plays that disc's music.
COMPILATION_REGION ?= US
COMPILATION_DISC ?= $(eval COMPILATION_DISC := $(shell for d in "$(ROMS)" "$(ROM_DIR)"; do python3 engine/tools/megacd/find_disc.py "$$d" $(COMPILATION_REGION) 2>/dev/null; done | head -1))$(COMPILATION_DISC)
MENU_IMAGE ?= $(or $(shell for f in "$(ROM_DIR)/menu.png" "$(ROM_DIR)/menu.bin" "$(ROMS)/menu.png" "$(ROMS)/menu.bin"; do [ -f "$$f" ] && echo "$$f" && break; done),-)
SONIC_COMPILATION_OUT ?= $(CURDIR)/build/ultimate-sh-$(COMPILATION_REGION)
# ROM_DIR: where your dumps are (any names, sub-folders, zips); prepare-roms finds them by content and puts
# them under their expected names in ROMS (../roms, beside the checkout), where every build reads them.
ROM_DIR ?= ../roms
ifeq ($(origin ROMS),undefined)
  ROMS := $(shell d="$${PS_ROMS_DIR:-../roms}"; mkdir -p "$$d" && cd "$$d" && pwd)
endif
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
	  'A fresh checkout needs only a folder of your own dumps:  make setup && make all ROM_DIR=/path/to/dumps' \
	  '' \
	  'make setup                     Python tools (engine/.venv) and a check of cmake, ninja, SDL2, m68k-elf binutils' \
	  'make all                       setup + prepare-roms + build-all + derived-all (each SKIPs what has no inputs)' \
	  'make prepare-roms              Find your dumps in ROM_DIR by content, put them in ROMS, make the lock-on images' \
	  'make build-all                 Every cartridge port whose ROM is in ROMS' \
	  'make build TITLE=shk1          One port (title code or folder);  make check TITLE=..  also self-checks it' \
	  'make play TITLE=shk1           Build if needed, then play' \
	  'make derived-all               Every ROM/disc output below whose inputs are there' \
	  '  make shk1-rom                SHK1 Genesis ROM (SH1 REV00 + SHK2)' \
	  '  make shbs-rom          SHBS lock-on ROM (SHK + SH1)' \
	  '  make shcd                 Native SHCD (the disc in ROMS/cd/shcd + its BIOS)' \
	  '  make shcd-reference       The same plus the emulator-core reference build' \
	  '  make sh-compilation       Ultimate SH Mega CD disc on your SHCD (COMPILATION_REGION=US|EU|JP, US default: 60 Hz)' \
	  '  make sh-compilation-all   the same for every region of SHCD you have' \
	  'make launcher-macos|linux|windows   SH Collection shelf from the built ports (on that OS)' \
	  'make package-macos|linux|windows    PixelSorcery app: DMG / DEB / Windows ZIP+EXE (on that OS; VERSION=x.y.z)' \
	  '' \
	  'Inputs: ROM_DIR = your dumps (any names, sub-folders, zips; default ../roms); ROMS = the prepared folder (../roms).' \
	  'Found by content: every cartridge, the SHCD discs, the Mega CD BIOS; optional ROM_DIR/menu.png (compilation).' \
	  'Overrides: DISC / SONICCD_DISC, COMPILATION_REGION, COMPILATION_DISC, MENU_IMAGE (- = plain menu), SONIC_COMPILATION_OUT, CORES.'

# Python tools in engine/.venv; the system tools are checked, not installed (TUTORIAL.md section 7 lists them).
PIP_PACKAGES = capstone numpy pillow librosa
setup:
	@test -x engine/.venv/bin/python || python3 -m venv engine/.venv
	@engine/.venv/bin/python -c 'import capstone, numpy, PIL, librosa' 2>/dev/null || engine/.venv/bin/pip install -q $(PIP_PACKAGES)
	@test -f engine/ext/Genesis-Plus-GX/core/system.c || git submodule update --init --recursive
	@missing=; for t in zsh cmake ninja cc; do command -v $$t >/dev/null || missing="$$missing $$t"; done; \
	 pkg-config --exists sdl2 2>/dev/null || command -v sdl2-config >/dev/null || missing="$$missing SDL2"; \
	 if [ -n "$$missing" ]; then echo "setup: missing:$$missing (see TUTORIAL.md section 7)" >&2; exit 1; fi
	@command -v m68k-elf-as >/dev/null && command -v m68k-elf-ld >/dev/null || \
	 echo 'setup: no m68k-elf binutils - only the Mega CD compilation disc needs them (macOS: brew install m68k-elf-binutils)'
	@echo 'setup: ready'

# a host-platform route: stop with a clear message on another system (uname -s starts with $(1))
only_on = case "$$(uname -s)" in $(1)*) ;; *) echo "$@ runs on $(2) only (this is $$(uname -s)); see TUTORIAL.md section 7" >&2; exit 2;; esac

# Aggregate proxy: the native ports plus every derived build whose required local inputs exist.
all: setup
	$(MAKE) prepare-roms ROM_DIR="$(ROM_DIR)"
	$(MAKE) build-all CORES=$(CORES)
	$(MAKE) derived-all CORES=$(CORES) ROMS="$(ROMS)" ROM_DIR="$(ROM_DIR)" SONICCD_DISC="$(SONICCD_DISC)" COMPILATION_DISC="$(COMPILATION_DISC)" MENU_IMAGE="$(MENU_IMAGE)" SONIC_COMPILATION_OUT="$(SONIC_COMPILATION_OUT)"

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
prepare-roms: setup
	engine/.venv/bin/python engine/tools/build_ui.py --prepare "$(ROM_DIR)"

# SHBS is the SHK + SH1 lock-on; it is a launcher title, not a separate recompilation.
shbs-rom:
	@test -f "$(ROMS)/shk.bin" -a -f "$(ROMS)/sh1.bin" || { echo "Need $(ROMS)/shk.bin and sh1.bin" >&2; exit 1; }
	engine/.venv/bin/python engine/tools/lockon.py "$(ROMS)/shk.bin" "$(ROMS)/sh1.bin" "$(ROMS)/shk_sh1.bin"

shbs-rom-if-available:
	@if [ -f "$(ROMS)/shk_sh1.bin" ]; then \
	  echo "SHBS ROM already present: $(ROMS)/shk_sh1.bin"; \
	elif [ -f "$(ROMS)/shk.bin" ] && [ -f "$(ROMS)/sh1.bin" ]; then \
	  $(MAKE) --no-print-directory shbs-rom ROMS="$(ROMS)"; \
	else echo "SKIP SHBS lock-on ROM: needs $(ROMS)/shk.bin + sh1.bin"; fi

# Genesis cartridge image derived from the player's SH1 REV00 and SHK2 dumps.
shk1-rom:
	./games/shk1/tools/ensure_rom.sh "$(ROMS)"
	@test -f "$(ROMS)/shk1.bin" || { echo "Need $(ROMS)/sh1.bin and shk_sh2.bin" >&2; exit 1; }
	@echo "Genesis ROM: $(ROMS)/shk1.bin"

shk1-rom-if-available:
	@if [ -f "$(ROMS)/sh1.bin" ] && [ -f "$(ROMS)/shk_sh2.bin" ]; then \
	  $(MAKE) --no-print-directory shk1-rom ROMS="$(ROMS)"; \
	else echo "SKIP SHK1 Genesis ROM: needs $(ROMS)/sh1.bin + shk_sh2.bin"; fi

# Ultimate SH: a bootable Mega CD compilation built from the owner's SHCD disc, menu art and ROMs.
sh-compilation:
	@test -n "$(COMPILATION_DISC)" -a -f "$(COMPILATION_DISC)" || { echo 'No $(COMPILATION_REGION) SHCD disc under $(ROMS) or $(ROM_DIR) (COMPILATION_REGION=JP|US|EU; found:' "$$(python3 engine/tools/megacd/find_disc.py '$(ROMS)' --list | cut -d' ' -f1 | sort -u | tr '\n' ' ')"')' >&2; exit 2; }
	@[ "$(COMPILATION_REGION)" != EU ] || echo 'note: a Europe disc runs at 50 Hz (games and music slower than the cartridges); COMPILATION_REGION=US for 60 Hz'
	@[ "$(COMPILATION_REGION)" != JP ] || echo 'note: a Japanese base does not work yet (its BIOS stops SH2 loading partway); COMPILATION_REGION=US'
	@test "$(MENU_IMAGE)" = - -o -f "$(MENU_IMAGE)" || { echo 'MENU_IMAGE=$(MENU_IMAGE): no such file (a 4x2 game-select .png or converted .bin; MENU_IMAGE=- or unset draws a plain menu)' >&2; exit 2; }
	@test -f "$(ROMS)/sh1.bin" || { echo "Need $(ROMS)/sh1.bin" >&2; exit 1; }
	ROM="$(ROMS)/sh1.bin" ROM2="$(ROMS)/sh2.bin" ROM3="$(ROMS)/sh3.bin" \
	  ROM4="$(ROMS)/shk.bin" ROM5="$(ROMS)/shk_sh1.bin" ROM7="$(ROMS)/sh3k.bin" \
	  ./engine/tools/megacd/ultimate_sh.sh "$(COMPILATION_DISC)" "$(MENU_IMAGE)" "$(SONIC_COMPILATION_OUT)"

sh-compilation-if-available:
	@if [ -n "$(COMPILATION_DISC)" ] && [ -f "$(COMPILATION_DISC)" ] && \
	   { [ "$(MENU_IMAGE)" = - ] || [ -f "$(MENU_IMAGE)" ]; } && [ -f "$(ROMS)/sh1.bin" ]; then \
	  $(MAKE) --no-print-directory sh-compilation ROMS="$(ROMS)" COMPILATION_DISC="$(COMPILATION_DISC)" MENU_IMAGE="$(MENU_IMAGE)" SONIC_COMPILATION_OUT="$(SONIC_COMPILATION_OUT)"; \
	else echo 'SKIP Ultimate SH Mega CD disc: MENU_IMAGE must exist or be -; needs a $(COMPILATION_REGION) SHCD disc under $(ROMS) (COMPILATION_REGION=JP|US|EU, or COMPILATION_DISC) and $(ROMS)/sh1.bin'; fi

shcd-if-available:
	@if [ -n "$(SONICCD_DISC)" ] && [ -f "$(SONICCD_DISC)" ] && \
	   ls "$(SCD_BIOS_DIR)"/scd_*.bin >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory shcd DISC="$(SONICCD_DISC)" CORES=$(CORES); \
	else echo 'SKIP SHCD native executable: set SONICCD_DISC and provide the matching BIOS in PS_SCD_BIOS (or the default BIOS folder)'; fi

# every region of SHCD found: build/ultimate-sh-JP, -US, -EU
sh-compilation-all:
	@regions=$$(python3 engine/tools/megacd/find_disc.py "$(ROMS)" --list | cut -d' ' -f1 | sort -u); \
	 [ -n "$$regions" ] || { echo 'No SHCD disc under $(ROMS)' >&2; exit 2; }; \
	 for r in $$regions; do $(MAKE) --no-print-directory sh-compilation ROMS="$(ROMS)" COMPILATION_REGION=$$r \
	   SONIC_COMPILATION_OUT="$(CURDIR)/build/ultimate-sh-$$r" MENU_IMAGE="$(MENU_IMAGE)" || exit 1; done

derived-all: shk1-rom-if-available shbs-rom-if-available sh-compilation-if-available shcd-if-available

launcher-macos:
	@$(call only_on,Darwin,macOS)
	./engine/tools/make_launcher.sh launchers/sh.txt

launcher-linux:
	@$(call only_on,Linux,Linux)
	./engine/tools/linux/make_shelf.sh sh

launcher-windows:
	@$(call only_on,MINGW,MSYS2 MinGW64)
	./engine/tools/windows/make_shelf.sh sh

package-macos:
	@$(call only_on,Darwin,macOS)
	./engine/tools/macapp/make_dmg.sh $(if $(VERSION),--version $(VERSION))

package-linux:
	./engine/tools/linux/make_deb.sh $(if $(VERSION),--version $(VERSION))

package-windows:
	@$(call only_on,MINGW,MSYS2 MinGW64)
	./engine/tools/windows/make_zip.sh $(if $(VERSION),--version $(VERSION))
