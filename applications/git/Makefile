.PHONY: all
all: git

include ../../mk/config.mk
include ../../mk/lib.mk

GIT_PORT_DIR := $(APPS_DIR)/git
GIT_UPSTREAM := $(THIRD_PARTY_DIR)/git
GIT_BUILD    := $(APPS_BUILD)/git
GIT_GEN_DIR  := $(GIT_BUILD)/generated
GIT_BIN      := $(GIT_BUILD)/git.bin

ZLIB_DIR     := $(THIRD_PARTY_DIR)/zlib
ZLIB_SOURCES := adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil
ZLIB_OBJECTS := $(addprefix $(GIT_BUILD)/zlib/,$(addsuffix .o,$(ZLIB_SOURCES)))

# Dynamically extract LIB_OBJS, BUILTIN_OBJS, and REFTABLE_OBJS from upstream Git Makefile
GIT_LIB_OBJS_REL := $(filter %.o,$(shell sed -n 's/^LIB_OBJS += //p' $(GIT_UPSTREAM)/Makefile))
GIT_BUILTIN_OBJS_REL := $(filter %.o,$(shell sed -n 's/^BUILTIN_OBJS += //p' $(GIT_UPSTREAM)/Makefile))
GIT_REFTABLE_OBJS_REL := $(filter %.o,$(shell sed -n 's/^REFTABLE_OBJS += //p' $(GIT_UPSTREAM)/Makefile))
GIT_XDIFF_OBJS_REL := xdiff/xdiffi.o xdiff/xemit.o xdiff/xhistogram.o xdiff/xmerge.o xdiff/xpatience.o xdiff/xprepare.o xdiff/xutils.o

GIT_COMPAT_OBJS_REL := \
	compat/regex/regex.o \
	compat/strcasestr.o \
	compat/memmem.o \
	compat/strlcpy.o \
	compat/strtoumax.o \
	compat/strtoimax.o \
	compat/qsort_s.o \
	compat/pread.o \
	compat/writev.o \
	compat/basename.o \
	compat/stub/procinfo.o \
	sha1dc_git.o \
	sha1dc/sha1.o \
	sha1dc/ubc_check.o \
	sha256/block/sha256.o \
	http.o \
	http-walker.o \
	remote-curl.o \
	common-main.o \
	git.o

GIT_UPSTREAM_OBJS := $(addprefix $(GIT_BUILD)/upstream/, \
	$(GIT_LIB_OBJS_REL) \
	$(GIT_BUILTIN_OBJS_REL) \
	$(GIT_REFTABLE_OBJS_REL) \
	$(GIT_XDIFF_OBJS_REL) \
	$(GIT_COMPAT_OBJS_REL))

GIT_PORT_OBJS := \
	$(GIT_BUILD)/port/ipo_curl.o \
	$(GIT_BUILD)/port/ipo_git_compat.o

GIT_GEN_HEADERS := \
	$(GIT_GEN_DIR)/command-list.h \
	$(GIT_GEN_DIR)/config-list.h \
	$(GIT_GEN_DIR)/hook-list.h \
	$(GIT_GEN_DIR)/version-def.h

GIT_COMMON_DEFINES := \
	-DIPO_OS \
	-DSHA1_DC \
	-DSHA1DC_NO_STANDARD_INCLUDES \
	-DSHA1DC_INIT_SAFE_HASH_DEFAULT=0 \
	-DSHA1DC_CUSTOM_INCLUDE_SHA1_C="\"git-compat-util.h\"" \
	-DSHA1DC_CUSTOM_INCLUDE_UBC_CHECK_C="\"git-compat-util.h\"" \
	-DSHA256_BLK \
	-DHAVE_GETRANDOM \
	-DHAVE_STRINGS_H \
	-DHAVE_ALLOCA_H \
	-DHAVE_PATHS_H \
	-DNO_OPENSSL \
	-DNO_PTHREADS \
	-DNO_ICONV \
	-DNO_GETTEXT \
	-DNO_EXPAT \
	-DNO_IPV6 \
	-DNO_UNIX_SOCKETS \
	-DNO_INITGROUPS \
	-DNO_SETITIMER \
	-DNO_STRUCT_ITIMERVAL \
	-DNO_SYMLINK_HEAD \
	-DNO_TRUSTABLE_FILEMODE \
	-DNO_ST_BLOCKS_IN_STRUCT_STAT \
	-DNO_NSEC \
	-DNO_PREAD \
	-DNO_WRITEV \
	-DNO_STRCASESTR \
	-DNO_MEMMEM \
	-DNO_STRLCPY \
	-DNO_STRTOUMAX \
	-DNO_MKDTEMP \
	-DNO_LIBGEN_H \
	-DNO_GETPAGESIZE \
	-DNO_POLL_H \
	-DNO_SYS_POLL_H \
	-DGIT_HOST_CPU="\"i686\"" \
	-DGIT_EXEC_PATH="\"/applications\"" \
	-DGIT_HTML_PATH="\"/share/doc/git-doc\"" \
	-DGIT_MAN_PATH="\"/share/man\"" \
	-DGIT_INFO_PATH="\"/share/info\"" \
	-DBINDIR="\"/applications\"" \
	-DPREFIX="\"/\"" \
	-DETC_GITCONFIG="\"/etc/gitconfig\"" \
	-DETC_GITATTRIBUTES="\"/etc/gitattributes\"" \
	-DDEFAULT_GIT_TEMPLATE_DIR="\"/share/git-core/templates\"" \
	-DFALLBACK_RUNTIME_PREFIX="\"/\"" \
	-DPAGER_ENV="\"LESS=FRX LV=-c\"" \
	-DDEFAULT_EDITOR="\"/applications/edit\"" \
	-DSHELL_PATH="\"/applications/sh\""

GIT_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -w \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	$(GIT_COMMON_DEFINES) \
	-I$(GIT_GEN_DIR) \
	-I$(GIT_PORT_DIR) \
	-I$(GIT_PORT_DIR)/compat \
	-I$(GIT_UPSTREAM) \
	-I$(GIT_UPSTREAM)/compat/regex \
	-I$(ZLIB_DIR) \
	-I$(LIB_DIR)/bearssl/inc \
	-include $(GIT_PORT_DIR)/git_ipo_compat.h

APP_ENTRY_OBJ := $(APPS_BUILD)/entry.o

GIT_WRAP_LDFLAGS := \
	-Wl,--wrap=main \
	-Wl,--wrap=exit \
	-Wl,--wrap=_exit \
	-Wl,--wrap=open \
	-Wl,--wrap=read \
	-Wl,--wrap=write \
	-Wl,--wrap=close \
	-Wl,--wrap=lseek \
	-Wl,--wrap=fstat \
	-Wl,--wrap=isatty

.PHONY: git clean-git
git: $(GIT_BIN)

$(APP_ENTRY_OBJ): $(APPS_DIR)/entry.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS)

$(GIT_GEN_DIR)/command-list.h: $(GIT_UPSTREAM)/command-list.txt
	@mkdir -p $(dir $@)
	sh $(GIT_UPSTREAM)/tools/generate-cmdlist.sh $(GIT_UPSTREAM) $@

$(GIT_GEN_DIR)/config-list.h: $(GIT_UPSTREAM)/tools/generate-configlist.sh
	@mkdir -p $(dir $@)
	sh $(GIT_UPSTREAM)/tools/generate-configlist.sh $(GIT_UPSTREAM) $@

$(GIT_GEN_DIR)/hook-list.h: $(GIT_UPSTREAM)/tools/generate-hooklist.sh
	@mkdir -p $(dir $@)
	sh $(GIT_UPSTREAM)/tools/generate-hooklist.sh $(GIT_UPSTREAM) $@

$(GIT_GEN_DIR)/version-def.h: $(GIT_UPSTREAM)/version-def.h.in $(GIT_UPSTREAM)/GIT-VERSION-GEN
	@mkdir -p $(dir $@)
	sh $(GIT_UPSTREAM)/GIT-VERSION-GEN $(GIT_UPSTREAM) $(GIT_UPSTREAM)/version-def.h.in $@

$(GIT_BUILD)/zlib/%.o: $(ZLIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -w -I$(ZLIB_DIR)

$(GIT_BUILD)/upstream/compat/regex/regex.o: $(GIT_UPSTREAM)/compat/regex/regex.c $(GIT_GEN_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(GIT_CFLAGS) -DGAWK -DNO_MBSUPPORT

$(GIT_BUILD)/upstream/remote-curl.o: $(GIT_UPSTREAM)/remote-curl.c $(GIT_GEN_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(GIT_CFLAGS) \
		-Dcmd_main=cmd_remote_curl \
		-Dsetup_git_directory_gently=ipo_remote_curl_setup_git_dir \
		-Dtrace2_cmd_name=ipo_remote_curl_trace2_cmd_name

$(GIT_BUILD)/upstream/run-command.o: $(GIT_UPSTREAM)/run-command.c $(GIT_GEN_HEADERS) $(GIT_PORT_DIR)/git_ipo_compat.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(GIT_CFLAGS) \
		-Dstart_command=git_orig_start_command \
		-Dfinish_command=git_orig_finish_command \
		-Dfinish_command_in_signal=git_orig_finish_command_in_signal \
		-Drun_command=git_orig_run_command \
		-Dpipe_command=git_orig_pipe_command \
		-Drun_processes_parallel=git_orig_run_processes_parallel \
		-Dstart_bg_command=git_orig_start_bg_command \
		-Dstart_async=git_orig_start_async \
		-Dfinish_async=git_orig_finish_async \
		-Din_async=git_orig_in_async \
		-Dasync_with_fork=git_orig_async_with_fork \
		-Dprepare_auto_maintenance=git_orig_prepare_auto_maintenance

$(GIT_BUILD)/upstream/%.o: $(GIT_UPSTREAM)/%.c $(GIT_GEN_HEADERS) $(GIT_PORT_DIR)/git_ipo_compat.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(GIT_CFLAGS)

$(GIT_BUILD)/port/%.o: $(GIT_PORT_DIR)/%.c $(GIT_GEN_HEADERS) $(GIT_PORT_DIR)/git_ipo_compat.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(GIT_CFLAGS)

$(GIT_BIN): $(GIT_PORT_DIR)/Makefile $(GIT_UPSTREAM_OBJS) $(GIT_PORT_OBJS) $(ZLIB_OBJECTS) $(APP_ENTRY_OBJ) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(BEARSSL_LIB_A)
	@mkdir -p $(dir $@)
	$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(APPS_DIR)/app.ld -Wl,-q \
		$(GIT_WRAP_LDFLAGS) \
		$(APP_ENTRY_OBJ) $(GIT_PORT_OBJS) $(GIT_UPSTREAM_OBJS) $(ZLIB_OBJECTS) $(BEARSSL_LIB_A) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles
	cp $@.elf $@
	rm -f $@.elf

clean-git:
	rm -rf $(GIT_BUILD)
