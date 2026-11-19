# ==============================================================================
# IPO_OS_Software - Userspace Libraries (libc, libm, libstdc++, libapp, libtcc1, libbearssl)
# ==============================================================================

.PHONY: lib clean-lib

USER_APP_API_SRCS := $(filter-out $(LIB_DIR)/bearssl/%, $(wildcard $(LIB_DIR)/*.c) $(wildcard $(LIB_DIR)/gl/*.c))
USER_APP_API_OBJS := $(patsubst $(LIB_DIR)/%.c, $(LIB_BUILD_DIR)/app_api/%.o, $(USER_APP_API_SRCS))

BEARSSL_SRCS := $(shell find $(LIB_DIR)/bearssl/src -name "*.c" -type f | sort)
BEARSSL_OBJS := $(patsubst $(LIB_DIR)/bearssl/src/%.c, $(LIB_BUILD_DIR)/bearssl/%.o, $(BEARSSL_SRCS))

USER_LIBC_OBJS := \
	$(LIB_BUILD_DIR)/libc/string.o \
	$(LIB_BUILD_DIR)/libc/unistd.o \
	$(LIB_BUILD_DIR)/libc/stdlib.o \
	$(LIB_BUILD_DIR)/libc/stdio.o \
	$(LIB_BUILD_DIR)/libc/sys.o \
	$(LIB_BUILD_DIR)/libc/init.o \
	$(LIB_BUILD_DIR)/libc/setjmp.o \
	$(LIB_BUILD_DIR)/libc/math.o \
	$(LIB_BUILD_DIR)/libc/dlfcn.o

USER_LIBM_OBJS := \
	$(LIB_BUILD_DIR)/libc/math.o

USER_LIBCXX_OBJS := \
	$(LIB_BUILD_DIR)/libc/cxx.o

lib: $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) $(APP_API_LIB_A) \
     $(BEARSSL_LIB_A) $(USER_CRT0) $(USER_CRTI) $(USER_CRTN) $(CRT1_O) $(LIBTCC1_A) $(LIBGCC_A)

$(LIBGCC_A):
	@mkdir -p $(dir $@)
	cp $$(gcc -m32 -print-libgcc-file-name) $@

$(USER_CRT0): $(LIB_DIR)/crt/crt0.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(USER_CRTI): $(LIB_DIR)/crt/crti.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(USER_CRTN): $(LIB_DIR)/crt/crtn.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(CRT1_O): $(APPS_DIR)/tcc/crt/crt1.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(LIB_BUILD_DIR)/libtcc1.o: $(APPS_DIR)/tcc/crt/libtcc1.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -O2 -fno-strict-aliasing -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie

$(LIBTCC1_A): $(LIB_BUILD_DIR)/libtcc1.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $<

$(LIB_BUILD_DIR)/libc/setjmp.o: $(LIB_DIR)/libc/setjmp.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(LIB_BUILD_DIR)/libc/%.o: $(LIB_DIR)/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -O2 -fno-strict-aliasing -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie -I$(INCLUDE_DIR)

$(LIB_BUILD_DIR)/libc/cxx.o: $(LIB_DIR)/libc/cxx.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ -m32 -O2 -fno-strict-aliasing -nostdinc -I$(INCLUDE_DIR) -I$(INCLUDE_DIR)/c++ -ffreestanding -fno-builtin -fno-rtti -fno-exceptions -fno-pic -fno-pie

$(USER_LIBC_A): $(USER_LIBC_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(USER_LIBM_A): $(USER_LIBM_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(USER_LIBCXX_A): $(USER_LIBCXX_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(LIB_BUILD_DIR)/app_api/%.o: $(LIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -O2 -fno-strict-aliasing -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie -I$(INCLUDE_DIR)

$(APP_LIB_A): $(USER_APP_API_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(APP_API_LIB_A): $(APP_LIB_A)
	@mkdir -p $(dir $@)
	cp -f $< $@

$(LIB_BUILD_DIR)/bearssl/%.o: $(LIB_DIR)/bearssl/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -Os -fno-strict-aliasing -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie -I$(LIB_DIR)/bearssl/src -I$(LIB_DIR)/bearssl/inc -I$(INCLUDE_DIR)

$(BEARSSL_LIB_A): $(BEARSSL_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

clean-lib:
	rm -rf $(LIB_BUILD_DIR)
