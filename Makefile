# TubeSearch: browse YouTube on RISC OS and play videos in Reel.
#
#   make              the RISC OS programs, the checks, out/!TubeSearch, out/TubeSearch-$(VERSION).zip
#   make test         the Linux tests (parsers, HTTP, end to end over TLS,
#                     and the same code under QEMU with RISC OS's alignment trap)
#   make fixtures     fetch the recorded YouTube answers the tests use
#   make clean
#
# Needs riscos-crossdev (RCD_DIR, default ../riscos-crossdev) at a tag, with
# its toolchain installed (make prebuilt there).
RCD_DIR ?= ../riscos-crossdev
include $(RCD_DIR)/riscos.mk
.DEFAULT_GOAL := all

VERSION := 0.2.0-rc1
DATE := 12 Oct 2026
VERDEFS := -DTUBE_VERSION='"$(VERSION)"' -DTUBE_DATE='"$(DATE)"'
OUT := out
CORE := src/api.c src/http.c src/yt.c src/json.c src/text.c
CORE_H := src/api.h src/http.h src/yt.h src/json.h src/text.h src/transport.h
APP_SRC := src/main.c src/ui.c src/thumbs.c src/transport_riscos.c $(CORE)
CLI_SRC := src/tscli.c src/transport_riscos.c $(CORE)
WARN := -Wall -Wextra -Wno-unused-parameter

all: $(OUT)/TubeSearch-$(VERSION).zip
test: test-host test-ui test-e2e test-qemu
check: checks test

# --- RISC OS ------------------------------------------------------------------
$(OUT)/tubesearch: $(APP_SRC) $(CORE_H) src/ui.h src/thumbs.h Makefile | riscos-toolchain-present
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) -std=gnu99 $(WARN) $(VERDEFS) $(RISCOS_LDFLAGS) -o $@ $(APP_SRC)

$(OUT)/tscli: $(CLI_SRC) $(CORE_H) | riscos-toolchain-present
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) -std=gnu99 $(WARN) $(RISCOS_LDFLAGS) -o $@ $(CLI_SRC)

checks: $(OUT)/tubesearch $(OUT)/tscli
	$(CHECK_ALL) $(OUT)/tubesearch
	$(CHECK_ALL) $(OUT)/tscli
	$(CHECK_UNALIGNED) -Isrc $(sort $(APP_SRC) $(CLI_SRC))

$(OUT)/!Sprites,ff9: app/tubesearch.png app/sm-tubesearch.png
	@mkdir -p $(OUT)
	$(PNG2SPRITE) $@ '!tubesearch=app/tubesearch.png' 'sm!tubesearc=app/sm-tubesearch.png'

app/tubesearch.png app/sm-tubesearch.png: tools/mkicons.py
	python3 tools/mkicons.py app

APPFILES := app/!Run,feb app/!Boot,feb app/Config,fff $(OUT)/!Help,fff
$(OUT)/!Help,fff: app/Help.txt
	@mkdir -p $(OUT)
	sed 's/@VERSION@/$(VERSION)/' $< > $@

$(OUT)/TubeSearch-$(VERSION).zip: $(OUT)/tubesearch $(OUT)/tscli $(OUT)/!Sprites,ff9 $(APPFILES) | checks
	rm -rf $(OUT)/!TubeSearch && mkdir -p $(OUT)/!TubeSearch
	$(RO_STRIP) -o $(OUT)/tubesearch.stripped $(OUT)/tubesearch
	$(RO_STRIP) -o $(OUT)/tscli.stripped $(OUT)/tscli
	$(ELF2AIF) -e $(OUT)/tubesearch.stripped $(OUT)/!TubeSearch/!RunImage,ff8 >/dev/null
	$(ELF2AIF) -e $(OUT)/tscli.stripped $(OUT)/!TubeSearch/tscli,ff8 >/dev/null
	cp $(APPFILES) $(OUT)/!Sprites,ff9 $(OUT)/!TubeSearch/
	cp COPYING $(OUT)/!TubeSearch/COPYING,fff
	rm -f $@
	$(ROZIP) $@ -C $(OUT) !TubeSearch
	@echo "$@: unzip on RISC OS and run !TubeSearch"

# --- Linux tests --------------------------------------------------------------
HOST_CFLAGS := -std=gnu99 $(WARN) -g -O1 -fsanitize=address,undefined -Isrc
FIX := tests/fixtures

fixtures: $(FIX)/next.json
$(FIX)/next.json:
	sh tests/fetch-fixtures.sh

$(OUT)/test_yt: tests/test_yt.c src/yt.c src/json.c src/text.c src/yt.h src/json.h src/text.h
	@mkdir -p $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/test_yt.c src/yt.c src/json.c src/text.c

$(OUT)/test_http: tests/test_http.c src/http.c src/http.h
	@mkdir -p $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/test_http.c src/http.c

$(OUT)/tscli-linux: src/tscli.c src/transport_openssl.c $(CORE) $(CORE_H)
	@mkdir -p $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ src/tscli.c src/transport_openssl.c $(CORE) -lssl -lcrypto

test-host: $(OUT)/test_yt $(OUT)/test_http fixtures
	$(OUT)/test_yt $(FIX)
	$(OUT)/test_http

# The desktop front end against riscos-crossdev's fake RISC OS (a fake Wimp,
# SpriteExtend and network): 32-bit, as on RISC OS.
$(OUT)/test_ui: Makefile tests/test_ui.c src/ui.c src/thumbs.c $(CORE) $(CORE_H) src/ui.h src/thumbs.h $(FAKE_SRC)
	@mkdir -p $(OUT)
	$(FAKE_CC) $(FAKE_CFLAGS) -std=gnu99 -Wno-unused-parameter -fsanitize=address,undefined -Isrc $(VERDEFS) \
	  -o $@ tests/test_ui.c src/ui.c src/thumbs.c $(CORE) $(FAKE_SRC)

test-ui: $(OUT)/test_ui fixtures
	$(OUT)/test_ui $(FIX)

test-e2e: $(OUT)/tscli-linux fixtures
	sh tests/e2e.sh $(OUT)/tscli-linux

# The portable code as ARM Linux programs under QEMU with the alignment
# trap: exit 135 means an unaligned access that would abort on RISC OS.
$(OUT)/test_yt.arm: tests/test_yt.c src/yt.c src/json.c src/text.c
	@mkdir -p $(OUT)
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) -std=gnu99 -Isrc -o $@ $^
$(OUT)/test_http.arm: tests/test_http.c src/http.c
	@mkdir -p $(OUT)
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) -std=gnu99 -Isrc -o $@ $^

test-qemu: $(OUT)/test_yt.arm $(OUT)/test_http.arm fixtures
	QEMU=$(QEMU_ALIGNTRAP) $(ALIGNTRAP) $(OUT)/test_yt.arm $(FIX)
	QEMU=$(QEMU_ALIGNTRAP) $(ALIGNTRAP) $(OUT)/test_http.arm

clean:
	rm -rf $(OUT)

.PHONY: all test check checks fixtures test-host test-ui test-e2e test-qemu clean
