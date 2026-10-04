PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/target/include),)
PS5_PAYLOAD_SDK := $(CURDIR)/vendor/ps5-payload-sdk-release/ps5-payload-sdk
endif
LLVM_BIN ?= /opt/homebrew/opt/llvm@17/bin
VERSION ?= 1.0
export LLVM_CONFIG := $(LLVM_BIN)/llvm-config
PS5_CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang

TARGET := fc27-squad-updater.elf
WEB_UI := src/web_ui.h
CFLAGS := -O2 -Wall -Wextra -Werror -D_BSD_SOURCE -std=gnu11 -I$(PS5_PAYLOAD_SDK)/target/include -DPAYLOAD_VERSION=\"$(VERSION)\"
LDFLAGS := -L$(PS5_PAYLOAD_SDK)/target/lib -Wl,--gc-sections
LIBS := /opt/ps5-payload-sdk/target/lib/libmicrohttpd.a -lkernel_sys -lkernel -lSceSystemService -lSceUserService -lSceFsInternalForVsh -lpthread -lSceNetCtl

.PHONY: all clean

all: $(TARGET)

$(WEB_UI): html/index.html html/logo.svg html/circle.png html/square.png html/save.jpg html/setting.png tools/embed_html.py
	python3 tools/embed_html.py html/index.html html/logo.svg html/circle.png html/square.png html/save.jpg html/setting.png $(WEB_UI)

$(TARGET): src/main.c $(WEB_UI)
	$(PS5_CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)
	$(LLVM_BIN)/llvm-strip --strip-all $@

clean:
	rm -f $(TARGET) $(WEB_UI)
