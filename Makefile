#
#  Makefile
#  AuxDim
#
#  Builds the kext with the Xcode Command Line Tools only (clang, ld, make, strip, zip).
#  Neither Xcode nor xcodebuild is needed.
#
#    ./bootstrap.sh            fetch MacKernelSDK and the Lilu SDK (once)
#    make                              Debug build   -> build/Debug/AuxDim.kext
#    make CONFIG=Release               Release build -> build/Release/AuxDim.kext
#    make dist                         Debug and Release zips in build/*/
#    make check                        compile only, no link (works with any recent clang, also on Linux)
#    make doctor                       report the toolchain and the fetched SDKs
#    make clean                        remove build/ (the fetched SDKs in Deps/ stay)
#    make distclean                    also remove the fetched SDKs (Deps/)
#
#  Troubleshooting: `make V=1` prints every command. If the final link fails with the
#  newest Xcode linker, `make LD_CLASSIC=1` retries with the classic one.
#
#  The compiler flags mirror the build settings of the Lilu plugins (WhateverGreen, ...).
#  Needs only GNU make 3.81, the version Apple ships.
#

# ---- project ---------------------------------------------------------------

NAME      := AuxDim
BUNDLE_ID := io.github.nickk19.AuxDim
VERSION   := 0.1.0
SRCDIR    := AuxDim

# ---- knobs (override on the command line) ------------------------------------

CONFIG    ?= Debug
MIN_OS    ?= 10.15
V         ?=
LD_CLASSIC ?=

CLANG     ?= clang
CLANGXX   ?= clang++
STRIP     ?= strip
NM        ?= nm
OTOOL     ?= otool
ZIP       ?= zip

# Fetched by bootstrap.sh (remove them with: ./bootstrap.sh --clean)
DEPS      ?= $(CURDIR)/Deps
SDK       ?= $(DEPS)/MacKernelSDK
LILU      ?= $(DEPS)/Lilu.kext/Contents/Resources

# ---- derived ---------------------------------------------------------------

ifeq ($(CONFIG),Debug)
CONFIG_FLAGS := -O0 -DDEBUG=1 -DAPPLE_KEXT_ASSERTIONS=1
endif
ifeq ($(CONFIG),Release)
CONFIG_FLAGS := -O2
endif
ifndef CONFIG_FLAGS
$(error CONFIG must be Debug or Release, got "$(CONFIG)")
endif

CONFIG_UC := $(shell echo $(CONFIG) | tr 'a-z' 'A-Z')

BUILD    := build/$(CONFIG)
OBJDIR   := $(BUILD)/obj
KEXT     := $(BUILD)/$(NAME).kext
BIN      := $(KEXT)/Contents/MacOS/$(NAME)
PLIST    := $(KEXT)/Contents/Info.plist
ARCHIVE  := $(NAME)-$(VERSION)-$(CONFIG_UC).zip

LIBKMOD  := $(SDK)/Library/x86_64/libkmod.a
PLUGIN_START := $(LILU)/Library/plugin_start.cpp

SRCS     := $(wildcard $(SRCDIR)/*.cpp)
OBJS     := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SRCS)) $(OBJDIR)/lilu_plugin_start.o
KMOD_C   := $(OBJDIR)/kmod_info.c
KMOD_OBJ := $(OBJDIR)/kmod_info.o

# Flags shared by C and C++. -nostdinc: only the kernel headers of MacKernelSDK and Lilu are used.
COMMON_FLAGS := \
	-target x86_64-apple-macos$(MIN_OS) \
	-mkernel -fno-builtin -fno-common \
	-fvisibility=hidden -fno-asynchronous-unwind-tables -fno-non-call-exceptions \
	-mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse -ftree-vectorize \
	-nostdinc \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-DPRODUCT_NAME=$(NAME) -DMODULE_VERSION=$(VERSION) \
	$(CONFIG_FLAGS) \
	-Wall -Wextra -Wno-unused-parameter -Wno-ossharedptr-misuse -Wno-vla -Wno-unknown-warning-option \
	-isystem $(LILU) -isystem $(SDK)/Headers

CFLAGS_ALL   := -x c $(COMMON_FLAGS)
CXXFLAGS_ALL := -x c++ -std=c++17 -nostdinc++ -fapple-kext -fno-exceptions -fno-rtti $(COMMON_FLAGS)

# -kext + x86_64: the linker leaves kernel symbols undefined on purpose, kxld binds them at load time.
# libkmod.a is given by path so that the SDK's own copy is the one linked, never the system's.
LDFLAGS_ALL := -target x86_64-apple-macos$(MIN_OS) -nostdlib -static -Xlinker -kext
ifneq ($(LD_CLASSIC),)
LDFLAGS_ALL += -Xlinker -ld_classic
endif

SED_VARS := -e 's|@NAME@|$(NAME)|g' -e 's|@BUNDLE_ID@|$(BUNDLE_ID)|g' -e 's|@VERSION@|$(VERSION)|g'

ifeq ($(V),1)
Q :=
say = @:
else
Q := @
say = @printf '  %-6s %s\n' '$(1)' '$(2)'
endif

# ---- targets ---------------------------------------------------------------

.PHONY: all kext zip dist check verify doctor clean distclean help preflight

all: kext

help:
	@echo "make [CONFIG=Debug|Release] [V=1]   build build/<CONFIG>/$(NAME).kext"
	@echo "make zip | dist                     zip the kext (dist: Debug and Release)"
	@echo "make check                          compile only, no link"
	@echo "make doctor                         show toolchain and SDK status"
	@echo "make clean                          remove build/ (the fetched SDKs in Deps/ stay)"
	@echo "make distclean                      also remove the fetched SDKs (Deps/)"

kext: $(BIN) $(PLIST)

zip: kext
	$(call say,ZIP,$(BUILD)/$(ARCHIVE))
	$(Q)cd $(BUILD) && rm -f *.zip && $(ZIP) -qry -FS $(ARCHIVE) $(NAME).kext

dist:
	$(Q)$(MAKE) --no-print-directory CONFIG=Debug zip
	$(Q)$(MAKE) --no-print-directory CONFIG=Release zip

preflight:
	@if [ ! -f "$(LILU)/Headers/kern_api.hpp" ] || [ ! -f "$(PLUGIN_START)" ]; then \
	  echo "ERROR: Lilu SDK not found in $(LILU)"; echo "       Run ./bootstrap.sh first."; exit 1; fi
	@if [ ! -d "$(SDK)/Headers" ] || [ ! -f "$(LIBKMOD)" ]; then \
	  echo "ERROR: MacKernelSDK not found in $(SDK)"; echo "       Run ./bootstrap.sh first."; exit 1; fi

# Compile only: same flags as the real build, nothing is written.
check: preflight
	@status=0; for src in $(SRCS); do \
	  echo "  CHECK  $$src"; \
	  $(CLANGXX) $(CXXFLAGS_ALL) -fsyntax-only $$src || status=1; \
	done; \
	echo "  CHECK  $(PLUGIN_START)"; \
	$(CLANGXX) $(CXXFLAGS_ALL) -w -fsyntax-only $(PLUGIN_START) || status=1; \
	if [ $$status -eq 0 ]; then echo "check OK"; else echo "check FAILED"; fi; exit $$status

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp Makefile | preflight
	$(call say,CXX,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CLANGXX) $(CXXFLAGS_ALL) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

# Third-party file: its warnings are not ours to fix.
$(OBJDIR)/lilu_plugin_start.o: $(PLUGIN_START) Makefile | preflight
	$(call say,CXX,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CLANGXX) $(CXXFLAGS_ALL) -w -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(KMOD_C): $(SRCDIR)/kmod_info.c.in Makefile
	$(call say,GEN,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)sed $(SED_VARS) $< > $@

$(KMOD_OBJ): $(KMOD_C) | preflight
	$(call say,CC,$<)
	$(Q)$(CLANG) $(CFLAGS_ALL) -c $< -o $@

$(BIN): $(OBJS) $(KMOD_OBJ) $(LIBKMOD)
	$(call say,LD,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CLANGXX) $(LDFLAGS_ALL) -o $@ $(OBJS) $(KMOD_OBJ) $(LIBKMOD)
ifeq ($(CONFIG),Release)
	$(call say,STRIP,$@)
	$(Q)$(STRIP) -x $@
endif
	$(Q)$(MAKE) --no-print-directory CONFIG=$(CONFIG) verify

# Sanity check of the linked binary. Only warns: it must never hide a kext that is in fact fine.
verify:
	@if command -v $(OTOOL) >/dev/null 2>&1 && ! $(OTOOL) -hv $(BIN) 2>/dev/null | grep -q KEXTBUNDLE; then \
	  echo "WARNING: $(BIN) is not a kext bundle (MH_KEXT_BUNDLE); did the linker ignore -kext?"; fi
	@if command -v $(NM) >/dev/null 2>&1; then \
	  bad=$$($(NM) -u $(BIN) 2>/dev/null | awk '{print $$NF}' | grep -E '^(dyld_stub_binder|___cxa_atexit|_atexit)$$' || true); \
	  if [ -n "$$bad" ]; then echo "WARNING: user space symbols referenced, the kext will not load:" $$bad; fi; fi

$(PLIST): $(SRCDIR)/Info.plist.in Makefile
	$(call say,PLIST,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)sed $(SED_VARS) $< > $@
	$(Q)if command -v plutil >/dev/null 2>&1; then plutil -lint $@ >/dev/null; fi

doctor:
	@echo "Toolchain"; \
	for t in $(CLANG) $(CLANGXX) ld $(STRIP) $(ZIP) sed plutil; do \
	  if command -v $$t >/dev/null 2>&1; then printf '  ok       %s\n' "$$(command -v $$t)"; \
	  else printf '  MISSING  %s\n' $$t; fi; done; \
	printf '  clang    %s\n' "$$($(CLANGXX) --version 2>&1 | head -n 1)"; \
	printf '  ld       %s\n' "$$(ld -v 2>&1 | head -n 1)"; \
	if command -v xcode-select >/dev/null 2>&1; then printf '  dev dir  %s\n' "$$(xcode-select -p 2>&1)"; fi; \
	echo "SDKs"; \
	if [ -f "$(LIBKMOD)" ]; then echo "  ok       MacKernelSDK ($(SDK))"; else echo "  MISSING  MacKernelSDK, run ./bootstrap.sh"; fi; \
	if [ -f "$(PLUGIN_START)" ]; then \
	  echo "  ok       Lilu SDK $$(sed -n '/<key>CFBundleVersion<\/key>/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' $(LILU)/../Info.plist 2>/dev/null | head -n 1)"; \
	else echo "  MISSING  Lilu SDK, run ./bootstrap.sh"; fi

clean:
	$(call say,CLEAN,build)
	$(Q)rm -rf build

distclean: clean
	$(call say,CLEAN,Deps)
	$(Q)./bootstrap.sh --clean >/dev/null

-include $(OBJS:.o=.d)
