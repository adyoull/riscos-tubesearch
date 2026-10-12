# Tube: browse YouTube on RISC OS and play videos in Reel.
#
#   make              the RISC OS programs, the checks, out/!Tube, out/Tube-$(VERSION).zip
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

VERSION := 0.1.0
OUT := out
CORE := src/api.c src/http.c src/yt.c src/json.c src/text.c
CORE_H := src/api.h src/http.h src/yt.h src/json.h src/text.h src/transport.h
APP_SRC := src/main.c src/ui.c src/thumbs.c src/transport_riscos.c $(CORE)
CLI_SRC := src/tubecli.c src/transport_riscos.c $(CORE)
WARN := -Wall -Wextra -Wno-unused-parameter

all: $(OUT)/Tube-$(VERSION).zip
test: test-host test-ui test-e2e test-qemu
check: checks test

# --- RISC OS ------------------------------------------------------------------
$(OUT)/tube: $(APP_SRC) $(CORE_H) src/ui.h src/thumbs.h | riscos-toolchain-present
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) -std=gnu99 $(WARN) $(RISCOS_LDFLAGS) -o $@ $(APP_SRC)

$(OUT)/tubecli: $(CLI_SRC) $(CORE_H) | riscos-toolchain-present
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) -std=gnu99 $(WARN) $(RISCOS_LDFLAGS) -o $@ $(CLI_SRC)

checks: $(OUT)/tube $(OUT)/tubecli
	$(CHECK_ALL) $(OUT)/tube
	$(CHECK_ALL) $(OUT)/tubecli
	$(CHECK_UNALIGNED) -Isrc $(sort $(APP_SRC) $(CLI_SRC))

$(OUT)/!Sprites,ff9: app/tube.png app/sm-tube.png
	@mkdir -p $(OUT)
	$(PNG2SPRITE) $@ '!tube=app/tube.png' 'sm!tube=app/sm-tube.png'

app/tube.png app/sm-tube.png: tools/mkicons.py
	python3 tools/mkicons.py app

APPFILES := app/!Run,feb app/!Boot,feb app/Config,fff $(OUT)/!Help,fff
$(OUT)/!Help,fff: app/Help.txt
	@mkdir -p $(OUT)
	sed 's/@VERSION@/$(VERSION)/' $< > $@

$(OUT)/Tube-$(VERSION).zip: $(OUT)/tube $(OUT)/tubecli $(OUT)/!Sprites,ff9 $(APPFILES) | checks
	rm -rf $(OUT)/!Tube && mkdir -p $(OUT)/!Tube
	$(RO_STRIP) -o $(OUT)/tube.stripped $(OUT)/tube
	$(RO_STRIP) -o $(OUT)/tubecli.stripped $(OUT)/tubecli
	$(ELF2AIF) -e $(OUT)/tube.stripped $(OUT)/!Tube/!RunImage,ff8 >/dev/null
	$(ELF2AIF) -e $(OUT)/tubecli.stripped $(OUT)/!Tube/tubecli,ff8 >/dev/null
	cp $(APPFILES) $(OUT)/!Sprites,ff9 $(OUT)/!Tube/
	cp COPYING $(OUT)/!Tube/COPYING,fff
	rm -f $@
	$(ROZIP) $@ -C $(OUT) !Tube
	@echo "$@: unzip on RISC OS and run !Tube"

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

$(OUT)/tubecli-linux: src/tubecli.c src/transport_openssl.c $(CORE) $(CORE_H)
	@mkdir -p $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ src/tubecli.c src/transport_openssl.c $(CORE) -lssl -lcrypto

test-host: $(OUT)/test_yt $(OUT)/test_http fixtures
	$(OUT)/test_yt $(FIX)
	$(OUT)/test_http

# The desktop front end against riscos-crossdev's fake RISC OS (a fake Wimp,
# SpriteExtend and network): 32-bit, as on RISC OS.
$(OUT)/test_ui: tests/test_ui.c src/ui.c src/thumbs.c $(CORE) $(CORE_H) src/ui.h src/thumbs.h $(FAKE_SRC)
	@mkdir -p $(OUT)
	$(FAKE_CC) $(FAKE_CFLAGS) -std=gnu99 -Wno-unused-parameter -fsanitize=address,undefined -Isrc \
	  -o $@ tests/test_ui.c src/ui.c src/thumbs.c $(CORE) $(FAKE_SRC)

test-ui: $(OUT)/test_ui fixtures
	$(OUT)/test_ui $(FIX)

test-e2e: $(OUT)/tubecli-linux fixtures
	sh tests/e2e.sh $(OUT)/tubecli-linux

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
