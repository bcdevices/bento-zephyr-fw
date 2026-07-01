#
# Copyright (c) 2019-2022 Blue Clover Devices
#
# SPDX-License-Identifier: Apache-2.0
#

PRJTAG := bento-zephyr-fw

# Makefile default shell is /bin/sh which does not implement `source`.
SHELL := /bin/bash

BASE_PATH := $(realpath .)
DIST := $(BASE_PATH)/dist

.PHONY: default
default: build

.PHONY: GIT-VERSION-FILE
GIT-VERSION-FILE:
	@sh ./GIT-VERSION-GEN
-include GIT-VERSION-FILE

VERSION_TAG := $(patsubst v%,%,$(GIT_DESC))

DOCKER_BUILD_ARGS :=
DOCKER_BUILD_ARGS += --network=host

DOCKER_RUN_ARGS :=
DOCKER_RUN_ARGS += --network=none

ZEPHYR_TAG := 4.3.0
ZEPHYR_SYSROOT := /usr/src/zephyr-$(ZEPHYR_TAG)/zephyr
ZEPHYR_USRROOT := $(HOME)/src/zephyr-$(ZEPHYR_TAG)/zephyr
ZEPHYR_LOCALROOT := $(BASE_PATH)/zephyrproject/zephyr

BOARD_ROOT := $(BASE_PATH)

BOARDS_APP :=
BOARDS_APP += bento/rp2350b/m33

APP_TARGETS := $(patsubst %,build.%/app/zephyr/zephyr.hex,$(BOARDS_APP))
BLINKY_TARGETS := $(patsubst %,build.%/blinky/zephyr/zephyr.hex,$(BOARDS_APP))

# Helper to source the Zephyr env from whichever root exists
define source-zephyr
	if [ -d $(ZEPHYR_USRROOT) ]; then source $(ZEPHYR_USRROOT)/zephyr-env.sh ; \
	elif [ -d $(ZEPHYR_SYSROOT) ]; then source $(ZEPHYR_SYSROOT)/zephyr-env.sh ; \
	elif [ -d $(ZEPHYR_LOCALROOT) ]; then source $(ZEPHYR_LOCALROOT)/zephyr-env.sh ; \
	else echo "No Zephyr"; fi
endef

# Apply out-of-tree Zephyr patches (idempotent; safe to run every build).
# Resolves the Zephyr root the same way the build does (user > sys > local).
.PHONY: apply-patches
apply-patches:
	@$(source-zephyr) && $(BASE_PATH)/patches/apply-patches.sh "$$ZEPHYR_BASE"

# FORCE ensures Make always runs the recipe, letting ninja handle
# incremental rebuild decisions.
FORCE:

build.%/app/zephyr/zephyr.hex: FORCE apply-patches
	@if [ -f build.$*/app/build.ninja ]; then \
	  echo "ninja -C build.$*/app" ; \
	  ninja -C build.$*/app ; \
	else \
	  $(source-zephyr) && \
	  west build --build-dir build.$*/app --pristine auto \
	    --board $* -s app $(WEST_BOARD_ROOT) ; \
	fi

build.%/blinky/zephyr/zephyr.hex: FORCE apply-patches
	@if [ -f build.$*/blinky/build.ninja ]; then \
	  echo "ninja -C build.$*/blinky" ; \
	  ninja -C build.$*/blinky ; \
	else \
	  $(source-zephyr) && \
	  west build --build-dir build.$*/blinky --pristine auto \
	    --board $* -s blinky $(WEST_BOARD_ROOT) ; \
	fi

.PHONY: versions
versions:
	@echo "GIT_DESC: $(GIT_DESC)"
	@echo "VERSION_TAG: $(VERSION_TAG)"

WEST_BOARD_ROOT := -- -DBOARD_ROOT=$(BOARD_ROOT)

JLINK := JLinkExe
JLINK_DEVICE := RP2350_M33_0
JLINK_SPEED := 5000
JLINK_SCRIPT := $(BASE_PATH)/boards/blueclover/bento2/support/bento2_rp2350b_run.JLinkScript

# Flash and auto-run via two-step: loadfile (normal halt), then pin-reset via JLinkScript
define jlink-flash-and-run
	printf 'connect\nloadfile $(1)\nq\n' > /tmp/jlink_flash.jlink
	$(JLINK) -device $(JLINK_DEVICE) -if SWD -speed $(JLINK_SPEED) -autoconnect 1 -CommanderScript /tmp/jlink_flash.jlink
	printf 'connect\nr\nq\n' > /tmp/jlink_run.jlink
	$(JLINK) -device $(JLINK_DEVICE) -if SWD -speed $(JLINK_SPEED) -autoconnect 1 -JLinkScriptFile $(JLINK_SCRIPT) -CommanderScript /tmp/jlink_run.jlink
endef

.PHONY: flash
flash: $(APP_TARGETS)
	$(call jlink-flash-and-run,$(BASE_PATH)/build.bento/rp2350b/m33/app/zephyr/zephyr.hex)

.PHONY: flash-blinky
flash-blinky: $(BLINKY_TARGETS)
	$(call jlink-flash-and-run,$(BASE_PATH)/build.bento/rp2350b/m33/blinky/zephyr/zephyr.hex)

.PHONY: build app
build app: $(APP_TARGETS)

.PHONY: blinky
blinky: $(BLINKY_TARGETS)

.PHONY: clean
clean:
	-rm -rf $(BINS) build build.*

.PHONY: prereq
prereq:
	pip3 install -r requirements.txt
	install -d zephyrproject
	cd zephyrproject && west init --mr v$(ZEPHYR_TAG)
	cd zephyrproject && west update
	pip3 install -r $(ZEPHYR_LOCALROOT)/scripts/requirements.txt

.PHONY: dist-prep
dist-prep:
	-install -d $(DIST)

.PHONY: dist-clean
dist-clean:
	-rm -rf $(DIST)

.PHONY: dist
dist: dist-clean dist-prep build
	install -m 666 build.blueclover_plt_demo_v2_nrf52832/app/zephyr/zephyr.hex dist/app-pltdemov2-$(VERSION_TAG).hex
	install -m 666 build.blueclover_plt_demo_v2_nrf52832/app/zephyr/zephyr.elf dist/app-pltdemov2-$(VERSION_TAG).elf
	install -m 666 build.blueclover_plt_demo_v2_nrf52832/app/zephyr/zephyr.map dist/app-pltdemov2-$(VERSION_TAG).map
	sed 's/{{BOARD}}/pltdemov2/g; s/{{VERSION}}/$(VERSION_TAG)/g' test-suites/suite-demo-board-zephyr.yaml.template > dist/suite-pltdemov2-board-zephyr-$(VERSION_TAG).yaml

.PHONY: deploy
deploy:
	pltcloud -t "$(API_TOKEN)" -f "dist/*" -v "v$(VERSION_TAG)" -p "$(PROJECT_UUID)"

.PHONY: docker
docker: dist-prep
	docker build $(DOCKER_BUILD_ARGS) -t "bcdevices/$(PRJTAG)" .
	-@docker rm -f "$(PRJTAG)-$(VERSION_TAG)" 2>/dev/null
	docker run  $(DOCKER_RUN_ARGS) --name "$(PRJTAG)-$(VERSION_TAG)"  -t "bcdevices/$(PRJTAG)" \
	 /bin/bash -c "make build dist"
	docker cp "$(PRJTAG)-$(VERSION_TAG):/usr/src/dist" $(BASE_PATH)
