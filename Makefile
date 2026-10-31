# Static radio for the Chumby One (i.MX233 / ARM926EJ-S, soft-float, kernel 2.6.28).
# Cross-built with zig as static musl. `toolchain/build_deps.sh` builds the
# sysroot (mbedTLS + libkusersync) and the trimmed ffmpeg into $(BUILD).
BUILD   ?= $(CURDIR)/deps
CC      := $(BUILD)/scripts/zcc
VERSION := $(shell cat VERSION)
REPO    := madewith-bestpractice/static-chumby
CFLAGS  := -O2 -DSTATIC_VERSION=\"$(VERSION)\" -Wall -Wextra -Wno-unused-parameter -I$(BUILD)/sysroot/include -Isrc/third_party/helix -DUSE_DEFAULT_STDLIB
LDFLAGS := -s
LDLIBS  := -L$(BUILD)/sysroot/lib -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -lm
HELIX   := $(wildcard src/third_party/helix/libhelix-mp3/*.c) $(wildcard src/third_party/helix/libhelix-aac/*.c) src/third_party/helix/helix_memory.c
SRCS    := $(wildcard src/*.c) src/third_party/qrcodegen.c $(HELIX)
OUT     := build/static-radio
ZIP     := build/static-chumby-$(VERSION).zip
HOST    ?= chumby
SSHCFG  ?=

all: $(OUT)

$(OUT): $(SRCS) $(wildcard src/*.h) VERSION
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDLIBS)

fonts:
	swift tools/genfont.swift > src/fonts.h.new && mv src/fonts.h.new src/fonts.h

# The release zip: unzip onto a FAT USB stick, or the updater fetches it.
#   userhook1        the chumby runs it at boot from a stick
#   README.txt
#   static/          the program (static-radio, ffmpeg, cacert.pem, boot.sh, VERSION, LICENSES/)
# plus build/latest.json, the manifest the updater reads from the latest release.
dist: $(OUT)
	rm -rf build/dist && mkdir -p build/dist/static/LICENSES
	cp $(OUT) build/dist/static/static-radio
	cp $(BUILD)/out/ffmpeg build/dist/static/ffmpeg
	curl -sfL https://curl.se/ca/cacert.pem -o build/dist/static/cacert.pem
	cp toolchain/boot.sh VERSION build/dist/static/
	cp dist/README.txt build/dist/README.txt && cp dist/README.txt build/dist/static/README.txt
	cp LICENSE build/dist/static/LICENSES/static-chumby-MIT.txt
	cp LICENSES/* THIRD_PARTY.md build/dist/static/LICENSES/
	cp toolchain/usb-userhook1 build/dist/userhook1
	chmod 755 build/dist/userhook1 build/dist/static/static-radio build/dist/static/ffmpeg build/dist/static/boot.sh
	rm -f $(ZIP) && cd build/dist && zip -qrX ../$(notdir $(ZIP)) userhook1 README.txt static
	printf '{"version":"%s","url":"https://github.com/$(REPO)/releases/download/v%s/%s","sha256":"%s","size":%s}\n' \
		"$(VERSION)" "$(VERSION)" "$(notdir $(ZIP))" "$$(shasum -a 256 $(ZIP) | cut -d' ' -f1)" "$$(wc -c < $(ZIP) | tr -d ' ')" > build/latest.json
	@ls -la $(ZIP) && cat build/latest.json

# Publish the release (needs gh, and push access to $(REPO)).
release: dist
	gh release create v$(VERSION) $(ZIP) build/latest.json --repo $(REPO) --title "Static for Chumby $(VERSION)" --notes-file dist/RELEASE_NOTES.md

# Development: copy the binary to an installed chumby over SSH and restart it.
# Upload beside the running binary, swap it in, THEN restart: boot.sh respawns
# the client 2 s after it exits, so killing first relaunched the old binary.
deploy: $(OUT)
	ssh $(SSHCFG) $(HOST) 'cat > /mnt/storage/static/static-radio.new && chmod 755 /mnt/storage/static/static-radio.new && mv /mnt/storage/static/static-radio.new /mnt/storage/static/static-radio && killall static-radio 2>/dev/null; sleep 2; killall -9 static-radio 2>/dev/null; true' < $(OUT)

# Development: install the whole dist folder on a chumby over SSH (as the stick's
# "Install on this chumby" would), then restart it.
install-ssh: dist
	cd build/dist && tar cf - static | ssh $(SSHCFG) $(HOST) 'rm -rf /tmp/static-inst && mkdir -p /tmp/static-inst && tar xf - -C /tmp/static-inst && \
		D=/mnt/storage/static && mkdir -p $$D && \
		for f in static-radio ffmpeg cacert.pem boot.sh VERSION README.txt; do cp /tmp/static-inst/static/$$f $$D/$$f.new && mv $$D/$$f.new $$D/$$f; done && \
		rm -rf $$D/LICENSES && cp -r /tmp/static-inst/static/LICENSES $$D/ && rm -rf /tmp/static-inst'
	ssh $(SSHCFG) $(HOST) 'mkdir -p /psp/rfs1 && { [ ! -e /psp/rfs1/userhook1 ] || grep -q "Static radio" /psp/rfs1/userhook1 || mv /psp/rfs1/userhook1 /psp/rfs1/userhook1.pre-static; } && \
		cat > /psp/rfs1/userhook1.new && chmod 755 /psp/rfs1/userhook1.new && mv /psp/rfs1/userhook1.new /psp/rfs1/userhook1' < toolchain/userhook1
	ssh $(SSHCFG) $(HOST) 'killall static-radio 2>/dev/null; sleep 2; killall -9 static-radio 2>/dev/null; true'

clean:
	rm -rf build

.PHONY: all fonts dist release deploy install-ssh clean
