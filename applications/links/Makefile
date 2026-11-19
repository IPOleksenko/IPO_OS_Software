include ../../mk/config.mk
include ../../mk/lib.mk

LINKS_DIR := $(APPS_DIR)/links
LINKS_UPSTREAM := $(LINKS_DIR)/upstream
LINKS_PORT := $(LINKS_DIR)/port
LINKS_BUILD := $(APPS_BUILD)/links
LINKS_BIN := $(LINKS_BUILD)/links.bin
LINKS_SOURCES := $(shell sed -n 's/^links_SOURCES=//p' $(LINKS_UPSTREAM)/Makefile.am)
LINKS_C_SOURCES := $(addprefix $(LINKS_UPSTREAM)/,$(filter %.c,$(LINKS_SOURCES)))
LINKS_PORT_SOURCES := $(wildcard $(LINKS_PORT)/*.c)
ZLIB_DIR := $(THIRD_PARTY_DIR)/zlib
ZLIB_SOURCES := adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil
ZLIB_HEADERS := $(wildcard $(ZLIB_DIR)/*.h)
ZLIB_OBJECTS := $(addprefix $(LINKS_BUILD)/zlib/,$(addsuffix .o,$(ZLIB_SOURCES)))

NS_SRC_DIR := $(THIRD_PARTY_DIR)/northstar_media
NS_WUFFS_DIR := $(THIRD_PARTY_DIR)/wuffs
WUFFS_OBJECTS := $(LINKS_BUILD)/media/wuffs.o $(LINKS_BUILD)/media/image_wuffs.o
MEDIA_C_OBJECTS := $(LINKS_BUILD)/media/mp4.o $(LINKS_BUILD)/media/aac.o

OH264_DIR := $(LINKS_DIR)/third_party/openh264
OH264_BUILD := $(LINKS_BUILD)/media/openh264
OH264_COMMON_SRCS := common_tables.cpp copy_mb.cpp cpu.cpp crt_util_safe_x.cpp \
	deblocking_common.cpp expand_pic.cpp intra_pred_common.cpp mc.cpp \
	memory_align.cpp sad_common.cpp utils.cpp welsCodecTrace.cpp
OH264_DECODER_CORE_SRCS := $(notdir $(wildcard $(OH264_DIR)/codec/decoder/core/src/*.cpp))
OH264_DECODER_PLUS_SRCS := welsDecoderExt.cpp
OH264_OBJECTS := \
	$(addprefix $(OH264_BUILD)/common/,$(OH264_COMMON_SRCS:.cpp=.o)) \
	$(addprefix $(OH264_BUILD)/dec_core/,$(OH264_DECODER_CORE_SRCS:.cpp=.o)) \
	$(addprefix $(OH264_BUILD)/dec_plus/,$(OH264_DECODER_PLUS_SRCS:.cpp=.o)) \
	$(LINKS_BUILD)/media/oh264_wrap.o
OH264_A := $(LINKS_BUILD)/media/libopenh264.a
OH264_CXXFLAGS := -m32 -O2 -std=gnu++11 -fno-exceptions -fno-rtti \
	-fno-pic -fno-pie -fno-builtin -fno-stack-protector -fno-threadsafe-statics \
	-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DNDEBUG \
	-I$(LINKS_PORT)/media/oh264_shim \
	-I$(OH264_DIR)/codec/api/wels -I$(OH264_DIR)/codec/common/inc \
	-I$(OH264_DIR)/codec/decoder/core/inc -I$(OH264_DIR)/codec/decoder/plus/inc \
	-I$(PROJECT_ROOT) -I$(INCLUDE_DIR) -I$(APPS_DIR)/include

LINKS_OBJECTS := \
	$(patsubst $(LINKS_UPSTREAM)/%.c,$(LINKS_BUILD)/upstream/%.o,$(LINKS_C_SOURCES)) \
	$(patsubst $(LINKS_PORT)/%.c,$(LINKS_BUILD)/port/%.o,$(LINKS_PORT_SOURCES)) \
	$(ZLIB_OBJECTS) $(WUFFS_OBJECTS) $(MEDIA_C_OBJECTS)
LINKS_HEADERS := $(addprefix $(LINKS_UPSTREAM)/,$(filter %.h %.inc,$(LINKS_SOURCES)))
LINKS_CFLAGS := $(APPS_CFLAGS) -std=gnu11 \
	-DHAVE_CONFIG_H -DIPO_OS \
	-I$(LINKS_PORT) -I$(LINKS_PORT)/include -I$(LINKS_PORT)/media -I$(LINKS_UPSTREAM) \
	-I$(NS_SRC_DIR) -I$(NS_SRC_DIR)/audio \
	-I$(ZLIB_DIR) -I$(LIB_DIR)/bearssl/inc
BEARSSL_LIB := $(BEARSSL_LIB_A)
APP_ENTRY_OBJ := $(APPS_BUILD)/entry.o

.PHONY: links links-objects
links: $(LINKS_BIN)
links-objects: $(LINKS_OBJECTS) $(OH264_A)

$(APP_ENTRY_OBJ): $(APPS_DIR)/entry.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS)

$(LINKS_BUILD)/zlib/%.o: $(ZLIB_DIR)/%.c $(ZLIB_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -I$(ZLIB_DIR)

$(LINKS_BUILD)/media/wuffs.o: $(NS_WUFFS_DIR)/wuffs-v0.4.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -DWUFFS_IMPLEMENTATION -Wno-unused-function -Wno-unused-parameter

$(LINKS_BUILD)/media/image_wuffs.o: $(NS_SRC_DIR)/image_wuffs.c $(LINKS_PORT)/media/glib.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -I$(LINKS_PORT)/media -iquote $(NS_SRC_DIR) -iquote $(NS_WUFFS_DIR) -Wno-unused-function

$(LINKS_BUILD)/media/mp4.o: $(NS_SRC_DIR)/mp4.c $(NS_SRC_DIR)/mp4.h $(LINKS_PORT)/media/glib.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -I$(LINKS_PORT)/media -iquote $(NS_SRC_DIR) -Wno-unused-function

$(LINKS_BUILD)/media/aac.o: $(NS_SRC_DIR)/audio/aac.c $(NS_SRC_DIR)/audio/aac.h $(LINKS_PORT)/media/glib.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -I$(LINKS_PORT)/media -iquote $(NS_SRC_DIR)/audio -Wno-unused-function

$(OH264_BUILD)/common/%.o: $(OH264_DIR)/codec/common/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(OH264_CXXFLAGS) -w

$(OH264_BUILD)/dec_core/%.o: $(OH264_DIR)/codec/decoder/core/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(OH264_CXXFLAGS) -w

$(OH264_BUILD)/dec_plus/%.o: $(OH264_DIR)/codec/decoder/plus/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(OH264_CXXFLAGS) -w

$(LINKS_BUILD)/media/oh264_wrap.o: $(LINKS_PORT)/media/oh264_wrap.cpp $(LINKS_PORT)/media/oh264_wrap.h
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(OH264_CXXFLAGS)

$(OH264_A): $(OH264_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(LINKS_BUILD)/upstream/%.o: $(LINKS_UPSTREAM)/%.c $(LINKS_HEADERS) $(LINKS_PORT)/config.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(LINKS_CFLAGS)

$(LINKS_BUILD)/port/%.o: $(LINKS_PORT)/%.c $(LINKS_PORT)/config.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(LINKS_CFLAGS)

$(LINKS_BIN): $(LINKS_DIR)/Makefile $(LINKS_OBJECTS) $(OH264_A) $(APP_ENTRY_OBJ) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(BEARSSL_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(APPS_DIR)/app.ld -Wl,-q \
		$(APP_ENTRY_OBJ) $(LINKS_OBJECTS) $(OH264_A) $(BEARSSL_LIB) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles
	cp $@.elf $@
	rm -f $@.elf
