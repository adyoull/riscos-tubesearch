# TubeSearch: browse YouTube on RISC OS and play videos in Reel.
#
#   make              the RISC OS programs, the checks, out/!TubeSearch, out/TubeSearch-$(VERSION).zip
#   make test         the Linux tests (parsers, HTTP, SHA3, the solver in
#                     QuickJS, the desktop front end on a fake RISC OS, end
#                     to end over TLS, and the same code under QEMU with
#                     RISC OS's alignment trap)
#   make fixtures     fetch the recorded YouTube answers the tests use
#   make clean
#
# Needs riscos-crossdev (RCD_DIR, default ../riscos-crossdev) at a tag, with
# its toolchain installed (make prebuilt there).
RCD_DIR ?= ../riscos-crossdev
include $(RCD_DIR)/riscos.mk
.DEFAULT_GOAL := all

VERSION := 0.2.0-rc13
DATE := 27 Oct 2026
VERDEFS := -DTUBE_VERSION='"$(VERSION)"' -DTUBE_DATE='"$(DATE)"'
OUT := out
EJS_VERSION := 0.8.0
EJS := ejs/$(EJS_VERSION)

CORE := src/api.c src/http.c src/yt.c src/json.c src/text.c src/solvejob.c src/ejsupd.c src/sha3.c src/proxy.c src/subs.c src/save.c
CORE_H := src/api.h src/http.h src/yt.h src/json.h src/text.h src/transport.h src/solvejob.h src/ejsupd.h src/sha3.h src/proxy.h src/subs.h src/alphabets.h src/save.h
SOLVER := src/solverun.c src/jsolve.c
QJS_DIR := third_party/quickjs
QJS_SRC := $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/dtoa.c
QJS_DEFS := -DCONFIG_VERSION='"$(shell cat $(QJS_DIR)/VERSION)"' -I$(QJS_DIR)
APP_SRC := src/main.c src/ui.c src/thumbs.c src/transport_riscos.c src/proxy_tunnel.c $(CORE)
CLI_SRC := src/tscli.c src/transport_riscos.c src/proxy_tunnel.c $(CORE) $(SOLVER)
WARN := -Wall -Wextra -Wno-unused-parameter

all: $(OUT)/TubeSearch-$(VERSION).zip
test: test-host test-solver test-ui test-e2e test-qemu
check: checks test

# --- RISC OS ------------------------------------------------------------------
$(OUT)/tubesearch: $(APP_SRC) $(CORE_H) src/ui.h src/thumbs.h Makefile | riscos-toolchain-present
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) -std=gnu99 $(WARN) $(VERDEFS) $(RISCOS_LDFLAGS) -o $@ $(APP_SRC)

# QuickJS (third party: its own warnings are not ours to fix), built with
# the same flags as everything else
$(OUT)/ro/%.o: $(QJS_DIR)/%.c | riscos-toolchain-present
	@mkdir -p $(OUT)/ro
	$(RO_CC) $(RISCOS_CFLAGS) $(QJS_DEFS) -w -c -o $@ $<
QJS_RO := $(patsubst $(QJS_DIR)/%.c,$(OUT)/ro/%.o,$(QJS_SRC))

$(OUT)/tscli: $(CLI_SRC) $(CORE_H) src/jsolve.h $(QJS_RO) | riscos-toolchain-present
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) -std=gnu99 $(WARN) -I$(QJS_DIR) $(RISCOS_LDFLAGS) -o $@ $(CLI_SRC) $(QJS_RO) -lm

checks: $(OUT)/tubesearch $(OUT)/tscli
	$(CHECK_ALL) $(OUT)/tubesearch
	$(CHECK_ALL) $(OUT)/tscli
	$(CHECK_UNALIGNED) -Isrc -I$(QJS_DIR) $(sort $(APP_SRC) $(CLI_SRC))

$(OUT)/!Sprites,ff9: app/tubesearch.png app/sm-tubesearch.png
	@mkdir -p $(OUT)
	$(PNG2SPRITE) $@ '!tubesearch=app/tubesearch.png' 'sm!tubesearc=app/sm-tubesearch.png'

app/tubesearch.png app/sm-tubesearch.png: tools/mkicons.py
	python3 tools/mkicons.py app

APPFILES := app/!Run,feb app/!Boot,feb app/Config,fff app/Solve,feb $(OUT)/!Help,fff $(OUT)/Licences,fff
$(OUT)/!Help,fff: app/Help.txt Makefile
	@mkdir -p $(OUT)
	sed 's/@VERSION@/$(VERSION)/' $< > $@
$(OUT)/Licences,fff: app/Licences.txt COPYING $(QJS_DIR)/LICENSE ejs/LICENSE
	@mkdir -p $(OUT)
	{ cat app/Licences.txt; echo; echo "==== TubeSearch (MIT) ===="; cat COPYING; \
	  echo; echo "==== QuickJS (MIT) ===="; cat $(QJS_DIR)/LICENSE; \
	  echo; echo "==== yt-dlp's solver scripts, yt-dlp/ejs (Unlicense) ===="; cat ejs/LICENSE; } > $@

$(OUT)/TubeSearch-$(VERSION).zip: $(OUT)/tubesearch $(OUT)/tscli $(OUT)/!Sprites,ff9 $(APPFILES) $(EJS)/yt.solver.lib.min.js $(EJS)/yt.solver.core.min.js | checks
	rm -rf $(OUT)/!TubeSearch && mkdir -p $(OUT)/!TubeSearch/Resources/ejs
	$(RO_STRIP) -o $(OUT)/tubesearch.stripped $(OUT)/tubesearch
	$(RO_STRIP) -o $(OUT)/tscli.stripped $(OUT)/tscli
	$(ELF2AIF) -e $(OUT)/tubesearch.stripped $(OUT)/!TubeSearch/!RunImage,ff8 >/dev/null
	$(ELF2AIF) -e $(OUT)/tscli.stripped $(OUT)/!TubeSearch/tscli,ff8 >/dev/null
	cp $(APPFILES) $(OUT)/!Sprites,ff9 $(OUT)/!TubeSearch/
	cp COPYING $(OUT)/!TubeSearch/COPYING,fff
	cp $(EJS)/yt.solver.lib.min.js $(OUT)/!TubeSearch/Resources/ejs/lib,fff
	cp $(EJS)/yt.solver.core.min.js $(OUT)/!TubeSearch/Resources/ejs/core,fff
	echo $(EJS_VERSION) > $(OUT)/!TubeSearch/Resources/ejs/Version,fff
	rm -f $@
	$(ROZIP) $@ -C $(OUT) !TubeSearch
	@echo "$@: unzip on RISC OS and run !TubeSearch"

# --- Linux tests --------------------------------------------------------------
HOST_CFLAGS := -std=gnu99 $(WARN) -g -O1 -fsanitize=address,undefined -Isrc
FIX := tests/fixtures

fixtures: $(FIX)/player_tv.json
$(FIX)/player_tv.json:
	sh tests/fetch-fixtures.sh

$(OUT)/test_yt: tests/test_yt.c src/yt.c src/json.c src/text.c src/yt.h src/json.h src/text.h src/alphabets.h
	@mkdir -p $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/test_yt.c src/yt.c src/json.c src/text.c

$(OUT)/test_http: tests/test_http.c src/http.c src/http.h
	@mkdir -p $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/test_http.c src/http.c

$(OUT)/test_subs: tests/test_subs.c src/subs.c src/subs.h src/yt.c src/json.c src/text.c
	@mkdir -p $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/test_subs.c src/subs.c src/yt.c src/json.c src/text.c

$(OUT)/test_proxy: tests/test_proxy.c src/proxy.c src/proxy_tunnel.c src/proxy.h
	@mkdir -p $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/test_proxy.c src/proxy.c src/proxy_tunnel.c

$(OUT)/test_sha3: tests/test_sha3.c src/sha3.c src/sha3.h
	@mkdir -p $(OUT)
	$(CC) $(HOST_CFLAGS) -o $@ tests/test_sha3.c src/sha3.c

# QuickJS for the host (no sanitizers: it's third party, and slow with them)
$(OUT)/host/%.o: $(QJS_DIR)/%.c
	@mkdir -p $(OUT)/host
	$(CC) -O2 -g -D_GNU_SOURCE $(QJS_DEFS) -w -c -o $@ $<
QJS_HOST := $(patsubst $(QJS_DIR)/%.c,$(OUT)/host/%.o,$(QJS_SRC))

$(OUT)/test_jsolve: tests/test_jsolve.c src/jsolve.c src/json.c src/jsolve.h $(QJS_HOST)
	@mkdir -p $(OUT)
	$(CC) -std=gnu99 $(WARN) -g -O1 -Isrc -I$(QJS_DIR) -o $@ tests/test_jsolve.c src/jsolve.c src/json.c $(QJS_HOST) -lm -lpthread

$(OUT)/tscli-linux: src/tscli.c src/transport_openssl.c src/proxy_tunnel.c $(CORE) $(SOLVER) $(CORE_H) src/jsolve.h $(QJS_HOST)
	@mkdir -p $(OUT)
	$(CC) -std=gnu99 $(WARN) -g -O1 -Isrc -I$(QJS_DIR) -o $@ src/tscli.c src/transport_openssl.c src/proxy_tunnel.c $(CORE) $(SOLVER) \
	  $(QJS_HOST) -lssl -lcrypto -lm -lpthread

test-host: $(OUT)/test_yt $(OUT)/test_http $(OUT)/test_proxy $(OUT)/test_subs $(OUT)/test_sha3 fixtures
	$(OUT)/test_yt $(FIX)
	$(OUT)/test_http
	$(OUT)/test_proxy
	$(OUT)/test_subs tests/subs
	$(OUT)/test_sha3 tests/sha3-vectors.txt ejs
	python3 tools/mkalphabets.py $(OUT)/alphabets.h && cmp $(OUT)/alphabets.h src/alphabets.h  # (made from the codecs)

# yt-dlp's solver on a real player, in QuickJS: node's answers
test-solver: $(OUT)/test_jsolve fixtures
	$(OUT)/test_jsolve $(FIX) $(EJS) -time

# The desktop front end against riscos-crossdev's fake RISC OS (a fake Wimp,
# SpriteExtend, network and JavaScript helper): 32-bit, as on RISC OS.
UI_SRC := src/ui.c src/thumbs.c $(CORE)
$(OUT)/test_ui: Makefile tests/test_ui.c $(UI_SRC) $(CORE_H) src/ui.h src/thumbs.h $(FAKE_SRC)
	@mkdir -p $(OUT)
	$(FAKE_CC) $(FAKE_CFLAGS) -std=gnu99 -Wno-unused-parameter -fsanitize=address,undefined -Isrc $(VERDEFS) \
	  -o $@ tests/test_ui.c $(UI_SRC) $(FAKE_SRC)

test-ui: $(OUT)/test_ui fixtures
	$(OUT)/test_ui $(FIX)

test-e2e: $(OUT)/tscli-linux fixtures
	sh tests/e2e.sh $(OUT)/tscli-linux

# The portable code as ARM Linux programs under QEMU with the alignment
# trap: exit 135 means an unaligned access that would abort on RISC OS.
# test_jsolve.arm runs the whole solve (QuickJS and the solver) that way.
$(OUT)/test_yt.arm: tests/test_yt.c src/yt.c src/json.c src/text.c
	@mkdir -p $(OUT)
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) -std=gnu99 -Isrc -o $@ $^
$(OUT)/test_subs.arm: tests/test_subs.c src/subs.c src/yt.c src/json.c src/text.c
	@mkdir -p $(OUT)
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) -std=gnu99 -Isrc -o $@ $^

$(OUT)/test_http.arm: tests/test_http.c src/http.c
	@mkdir -p $(OUT)
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) -std=gnu99 -Isrc -o $@ $^
$(OUT)/test_sha3.arm: tests/test_sha3.c src/sha3.c
	@mkdir -p $(OUT)
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) -std=gnu99 -Isrc -o $@ $^
$(OUT)/test_jsolve.arm: tests/test_jsolve.c src/jsolve.c src/json.c $(QJS_SRC)
	@mkdir -p $(OUT)
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) -std=gnu99 -D_GNU_SOURCE -Isrc $(QJS_DEFS) -w -o $@ $^ -lm -lpthread

test-qemu: $(OUT)/test_yt.arm $(OUT)/test_subs.arm $(OUT)/test_http.arm $(OUT)/test_sha3.arm $(OUT)/test_jsolve.arm fixtures
	QEMU=$(QEMU_ALIGNTRAP) $(ALIGNTRAP) $(OUT)/test_yt.arm $(FIX)
	QEMU=$(QEMU_ALIGNTRAP) $(ALIGNTRAP) $(OUT)/test_subs.arm tests/subs
	QEMU=$(QEMU_ALIGNTRAP) $(ALIGNTRAP) $(OUT)/test_http.arm
	QEMU=$(QEMU_ALIGNTRAP) $(ALIGNTRAP) $(OUT)/test_sha3.arm tests/sha3-vectors.txt ejs
	QEMU=$(QEMU_ALIGNTRAP) $(ALIGNTRAP) $(OUT)/test_jsolve.arm $(FIX) $(EJS) -time

clean:
	rm -rf $(OUT)

.PHONY: all test check checks fixtures test-host test-solver test-ui test-e2e test-qemu clean
