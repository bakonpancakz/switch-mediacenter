#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	MediaCenter
BUILD		:=	build
VENDOR		:=	vendor
SOURCES		:=	source source/ui source/core source/books source/media source/spotify source/tabs \
				$(VENDOR)/cspot/src $(VENDOR)/cspot/generated/protobuf $(VENDOR)/bell/src \
				$(VENDOR)/nanopb $(VENDOR)/cjson $(VENDOR)/sqlite
DATA		:=	data
INCLUDES	:=	source $(VENDOR)/cspot/include $(VENDOR)/cspot/generated $(VENDOR)/bell/include \
				$(VENDOR)/nanopb $(VENDOR)/cjson $(VENDOR)/sqlite
ROMFS		:=	romfs

APP_TITLE	:=	Media Center
APP_AUTHOR	:=	bakonpancakz
APP_VERSION	:=	2.0.0
ICON		:=	icon.jpg

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
PKGS	:=	mpv sdl2 SDL2_ttf SDL2_image libarchive liblz4 libavformat libavcodec libswscale libswresample libavutil libturbojpeg libavif libwebpdemux libcurl

ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# SQLite: our own file layer (source/core/sqlite_vfs.cpp), one read-only connection behind our own lock
SQLITE_DEFINES	:=	-DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_LOAD_EXTENSION \
				-DSQLITE_OMIT_WAL -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED \
				-DSQLITE_DQS=0 -DSQLITE_OMIT_SHARED_CACHE

DEFINES	:=	-D__SWITCH__ -DBELL_ONLY_CJSON -DBELL_DISABLE_FMT -DPB_ENABLE_MALLOC -DPB_FIELD_32BIT $(SQLITE_DEFINES)

# devkitPro may not be at /opt/devkitpro: PKGCONF_FLAGS (scripts/devkitpro-env.sh) remaps the portlibs' prefix
PKGCONF	:=	$(PREFIX)pkg-config $(PKGCONF_FLAGS)

CFLAGS	:=	`$(PKGCONF) --cflags $(PKGS)` -g -Wall -O2 -ffunction-sections -fdata-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -I$(PORTLIBS)/include/tremor
# source paths baked into the binary (__FILE__ in the vendored libraries' log
# messages, debug info) are relative to the project, not this machine's folders
CFLAGS	+=	-ffile-prefix-map=$(TOPDIR)/= -ffile-prefix-map=$(shell cygpath -m $(TOPDIR) 2>/dev/null)/=

CXXFLAGS	:= $(CFLAGS) -std=gnu++20 -fexceptions -frtti -Wno-unused-variable -Wno-sign-compare -Wno-reorder

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)

# MuPDF goes first so its HarfBuzz calls resolve to the system HarfBuzz (which
# SDL_ttf, libass and FreeType also use): libmupdf-third-nohb.a is the portlib's
# libmupdf-third.a minus its own, older HarfBuzz, made in the build directory.
LIBS	:=	-L$(CURDIR) -lmupdf -lmupdf-third-nohb 			`$(PKGCONF) --libs --static $(PKGS)` -lvorbisidec -logg -lmbedtls -lmbedx509 -lmbedcrypto -lnx

#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq (,$(wildcard $(TOPDIR)/$(ICON)))
	export APP_ICON := $(TOPDIR)/$(ICON)
else
	export APP_ICON := $(LIBNX)/default_icon.jpg
endif

export NROFLAGS += --icon=$(APP_ICON)
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf	:	$(OFILES) libmupdf-third-nohb.a

libmupdf-third-nohb.a	:	$(PORTLIBS)/lib/libmupdf-third.a
	@cp $< $@
	@$(PREFIX)ar d $@ $$($(PREFIX)ar t $@ | grep '^hb-')
	@echo made $@ without HarfBuzz

$(OFILES_SRC)	: $(HFILES_BIN)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
