CC := $(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-gcc
AS := nasm

NEOOS_GITREV := $(shell git describe --always --dirty --tags 2>/dev/null || echo unknown)
CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -Wall -Wextra -std=gnu11 -O2 -Ikernel -Ishared -DNEOOS_GITREV='"$(NEOOS_GITREV)"'
ASFLAGS := -f elf64

ifdef NEOOS_DEBUG_STOP_WINDOW
CFLAGS += -DNEOOS_DEBUG_STOP_WINDOW
endif

ifdef DEBUG_HEAP
CFLAGS += -DNEOOS_DEBUG_HEAP
endif

# Skips the selftests a person would NOTICE underneath an interactive
# session: the six LOOPER processes that print forever, and
# vt_stress_selftest, which clears the screen and flips VTs. Everything
# else only writes to the serial log, which is a file here.
ifeq ($(QUIET),1)
CFLAGS += -DNEOOS_QUIET_BOOT
endif

ifdef DEBUG_LOCKSTAT
CFLAGS += -DNEOOS_DEBUG_LOCKSTAT
endif

# Fire the LAPIC timer N times faster while keeping timer_ticks() at
# 100 Hz, so preemption windows widen without disturbing the clock.
ifdef DEBUG_HZ
CFLAGS += -DNEOOS_DEBUG_HZ=$(DEBUG_HZ)
endif

# Fail one pmm_alloc in N, deterministically by count, so the
# "if (!frame) ..." paths that never run in a 128 MiB VM actually run.
ifdef DEBUG_PMMFAIL
CFLAGS += -DNEOOS_DEBUG_PMMFAIL=$(DEBUG_PMMFAIL)
endif

ifeq ($(MAKECMDGOALS),test)
CFLAGS += -DNEOOS_TEST_HOOKS
endif

BUILD_DIR := build
ISO_DIR := iso

# One entry per kernel subdirectory. Kept as an explicit list rather
# than a recursive wildcard so that adding a directory is a deliberate
# act: a stray .c file in a new folder should fail to link, not get
# picked up silently.
KERNEL_DIRS := kernel kernel/arch kernel/drivers/video kernel/drivers/input \
	kernel/drivers/block kernel/drivers/char kernel/drivers/irq kernel/drivers/acpi \
	kernel/drivers/pci kernel/drivers/virtio kernel/drivers/net \
	kernel/tty kernel/ipc kernel/smp \
	kernel/syscall kernel/mm kernel/fs kernel/sched kernel/sync kernel/net kernel/lib
C_SOURCES := $(foreach d,$(KERNEL_DIRS),$(wildcard $(d)/*.c))
# Every kernel header, as a coarse prerequisite for every object. Without
# this, editing a .h leaves stale .o files behind and a genuinely broken
# tree can appear to build clean.
C_HEADERS := $(foreach d,$(KERNEL_DIRS),$(wildcard $(d)/*.h))
C_OBJECTS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/isr_stubs.o $(BUILD_DIR)/context_switch.o $(BUILD_DIR)/syscall_entry.o $(BUILD_DIR)/fork_trampoline.o $(BUILD_DIR)/sigframe.o $(BUILD_DIR)/ap_trampoline.o

.PHONY: all build iso run shell shell-serial test fresh-disks clean disk-image font-regen font-check render-check lock-check

all: build

build: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR)/boot.o: boot/boot.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) boot/boot.asm -o $(BUILD_DIR)/boot.o

$(BUILD_DIR)/gdt_flush.o: kernel/arch/gdt_flush.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/arch/gdt_flush.asm -o $(BUILD_DIR)/gdt_flush.o

$(BUILD_DIR)/isr_stubs.o: kernel/arch/isr.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/arch/isr.asm -o $(BUILD_DIR)/isr_stubs.o

$(BUILD_DIR)/context_switch.o: kernel/arch/context_switch.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/arch/context_switch.asm -o $(BUILD_DIR)/context_switch.o

$(BUILD_DIR)/syscall_entry.o: kernel/syscall/syscall_entry.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/syscall/syscall_entry.asm -o $(BUILD_DIR)/syscall_entry.o

$(BUILD_DIR)/fork_trampoline.o: kernel/arch/fork_trampoline.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/arch/fork_trampoline.asm -o $(BUILD_DIR)/fork_trampoline.o

$(BUILD_DIR)/sigframe.o: kernel/ipc/sigframe.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/ipc/sigframe.asm -o $(BUILD_DIR)/sigframe.o

$(BUILD_DIR)/ap_trampoline.o: kernel/arch/ap_trampoline.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/arch/ap_trampoline.asm -o $(BUILD_DIR)/ap_trampoline.o

$(BUILD_DIR)/%.o: kernel/%.c $(C_HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.elf: $(ASM_OBJECTS) $(C_OBJECTS) linker.ld
	$(CC) -T linker.ld -o $(BUILD_DIR)/kernel.elf -ffreestanding -O2 -nostdlib $(ASM_OBJECTS) $(C_OBJECTS) -lgcc

iso: $(BUILD_DIR)/kernel.elf
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD_DIR)/kernel.elf $(ISO_DIR)/boot/kernel.elf
	cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD_DIR)/neoos.iso $(ISO_DIR)

DISK_IMG := $(BUILD_DIR)/disk.img
DISK2_IMG := $(BUILD_DIR)/disk2.img
DISK_SRC := $(BUILD_DIR)/disk-src

LIB_DIR := lib
LIB_BUILD := $(BUILD_DIR)/lib
LIB_SOURCES := $(wildcard $(LIB_DIR)/*.c)
LIB_OBJECTS := $(patsubst $(LIB_DIR)/%.c,$(LIB_BUILD)/%.o,$(LIB_SOURCES))

USERLAND_DIR := userland
USERLAND_BUILD := $(BUILD_DIR)/userland
USER_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -msse3 -mssse3 -msse4.1 -msse4.2 -mcmodel=large -fno-pic -ftls-model=local-exec -static -nostdlib -Wall -Wextra -std=gnu11 -O2 -I$(LIB_DIR)/include -Ishared

# ---- musl -----------------------------------------------------------
#
# musl is the real C library; lib/ is the NeoOS-native extension layer
# beside it. A musl program reaches the kernel through the shim in
# third_party/shim, which translates Linux syscall numbers into NeoOS's
# -- see docs/stdlib.md.
#
# Same code model as everything else in userland: programs link at
# 0x200000000000, so -mcmodel=large is not optional.
MUSL_DIR   := third_party/musl
MUSL_LIB   := $(MUSL_DIR)/lib/libc.a
MUSL_CFLAGS := -static -nostdlib -nostdinc -ffreestanding \
	-mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
	-isystem $(MUSL_DIR)/include -isystem $(MUSL_DIR)/arch/x86_64 \
	-isystem $(MUSL_DIR)/arch/generic -isystem $(MUSL_DIR)/obj/include

# Installs the shim, then builds musl. Both steps are idempotent, and
# the shim sources live in third_party/shim so the submodule itself
# stays a pristine checkout apart from the two files copied in.
# Depends on the SHIM SOURCES, not just on its own absence. Without
# that, editing the shim left the built libc.a alone: the execve mapping
# added for BusyBox sat in third_party/shim for hours while every
# musl-linked program kept calling the old library, and BusyBox's shell
# reported "Function not implemented" for every external command. The
# mapping was correct the whole time; the build simply never picked it
# up. `make musl` is idempotent, so rebuilding on a shim edit is cheap.
$(MUSL_LIB): $(wildcard third_party/shim/*.c third_party/shim/*.h \
                        third_party/shim/*.s third_party/shim/apply.sh)
	third_party/shim/apply.sh
	cd $(MUSL_DIR) && ./configure --target=x86_64 --disable-shared \
		CC=$(CC) AR=$(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-ar \
		RANLIB=$(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-ranlib \
		CFLAGS="-mcmodel=large -fno-pic -mno-red-zone -O2" >/dev/null
	$(MAKE) -C $(MUSL_DIR) -j$(shell nproc) >/dev/null

.PHONY: musl
musl: $(MUSL_LIB)

# ---- busybox ---------------------------------------------------------
#
# BB1. A pristine upstream submodule; everything NeoOS-specific lives in
# third_party/busybox-config, mirroring third_party/shim. apply.sh runs
# allnoconfig + a fragment rather than checking in a whole .config, so a
# version bump takes upstream's default for options the fragment does
# not name.
#
# Not built by `all`: it is a minute of compile that the kernel's own
# test cycle does not need. `make busybox` builds it, and the disk image
# picks it up if it is there.
BUSYBOX_DIR := third_party/busybox
BUSYBOX_BIN := $(BUSYBOX_DIR)/busybox

$(BUSYBOX_BIN): $(MUSL_LIB) third_party/busybox-config/neoos.fragment \
                third_party/busybox-config/apply.sh $(USERLAND_DIR)/user.ld
	third_party/busybox-config/apply.sh
	$(MAKE) -C $(BUSYBOX_DIR) -j$(shell nproc)

.PHONY: busybox
busybox: $(BUSYBOX_BIN)

# ---- ports -----------------------------------------------------------
#
# Third-party applications (ports/README.md). Built on request, NOT by
# `all` and NOT part of the boot suite: the suite and the gauntlet are a
# KERNEL regression harness, and a broken application must not be able
# to make them fail.
PORTS_DIR   := ports
AV_DIR      := $(PORTS_DIR)/3d-ascii-viewer
AV_SHIM     := $(AV_DIR)/ncurses-shim
AV_SRCS     := $(wildcard $(AV_DIR)/upstream/src/*.c) $(AV_SHIM)/ncurses_shim.c
AV_BIN      := $(BUILD_DIR)/ports/3d-ascii-viewer

# The upstream program is built UNMODIFIED, against a seventeen-function
# ncurses replacement (ncurses-shim) rather than a ported ncurses -- see
# the header there for why. -I puts the shim ahead of the search path so
# its <ncurses.h> is the one found.
$(AV_BIN): $(AV_SRCS) $(MUSL_LIB) $(USERLAND_DIR)/user.ld
	@mkdir -p $(BUILD_DIR)/ports
	$(CC) $(MUSL_CFLAGS) -I$(AV_SHIM) -T $(USERLAND_DIR)/user.ld -z noexecstack \
		-o $@ $(MUSL_DIR)/lib/crt1.o $(AV_SRCS) \
		-L$(MUSL_DIR)/lib -lc -lgcc

.PHONY: ports
ports: $(AV_BIN)
	@echo "ports: 3d-ascii-viewer built ($$(stat -c%s $(AV_BIN)) bytes)"

# Smoke-test the ports in their OWN boot, with an INITTAB of their own.
# Separate from `make test` on purpose (ports/README.md): the boot suite
# is a kernel regression harness, and an application must not be able to
# fail it.
#
# The viewer is interactive, so it is run with -d: render for a few
# seconds and exit. What is asserted is that it loads, runs to
# completion with status 0, and asks the kernel for nothing the kernel
# does not have -- a [shim] ENOSYS line is a failure here, because it
# means the port is silently missing a primitive.
.PHONY: ports-test
ports-test: ports iso disk-image
	@printf '%s\n' \
	  '# generated by `make ports-test`' \
	  'wait /bin/term.nex /bin/av.nex av --color -d 3 /usr/share/models/box-in-box.obj' \
	  > $(BUILD_DIR)/disk-src/inittab.ports
	@mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/disk-src/inittab.ports ::etc/inittab
	-@timeout $(BOOT_TIMEOUT) qemu-system-x86_64 $(QEMU_COMMON) \
		-display none -serial file:$(BUILD_DIR)/ports.log > /dev/null 2>&1
	@if grep -q 'PANIC\|\[exception\]' $(BUILD_DIR)/ports.log; then \
	    echo "PORTS: the KERNEL did not survive"; \
	    grep -E 'PANIC|\[exception\]' $(BUILD_DIR)/ports.log; exit 1; fi
	@if grep -q '\[shim\] ENOSYS' $(BUILD_DIR)/ports.log; then \
	    echo "PORTS: a port asked for a syscall NeoOS does not have:"; \
	    grep '\[shim\] ENOSYS' $(BUILD_DIR)/ports.log | sort -u; exit 1; fi
	@if ! grep -q '\[term\] child exited, status 0' $(BUILD_DIR)/ports.log; then \
	    echo "PORTS: 3d-ascii-viewer did not exit cleanly"; \
	    grep -E '\[term\]|rejected|not found' $(BUILD_DIR)/ports.log | tail -5; exit 1; fi
	@echo "PORTS PASSED: 3d-ascii-viewer rendered and exited 0, no missing syscalls"


# login links against musl, not libneoos, for one reason: crypt().
# musl ships SHA-256-crypt, SHA-512-crypt, MD5 and bcrypt back-ends, all
# pure computation with no files and no randomness -- so NeoOS gets real
# password hashing with nothing ported and nothing hand-rolled.
$(USERLAND_BUILD)/LOGIN.ELF: $(USERLAND_DIR)/musl/login.c $(USERLAND_DIR)/user.ld $(MUSL_LIB)
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(MUSL_CFLAGS) -T $(USERLAND_DIR)/user.ld -z noexecstack -o $@ \
		$(MUSL_DIR)/lib/crt1.o $(USERLAND_DIR)/musl/login.c \
		-L$(MUSL_DIR)/lib -lc -lgcc

$(USERLAND_BUILD)/MUSLFORK.ELF: $(USERLAND_DIR)/musl/forkchild.c $(USERLAND_DIR)/user.ld $(MUSL_LIB)
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(MUSL_CFLAGS) -T $(USERLAND_DIR)/user.ld -z noexecstack -o $@ \
		$(MUSL_DIR)/lib/crt1.o $(USERLAND_DIR)/musl/forkchild.c \
		-L$(MUSL_DIR)/lib -lc -lgcc

$(USERLAND_BUILD)/MUSLHELO.ELF: $(USERLAND_DIR)/musl/hello.c $(USERLAND_DIR)/user.ld $(MUSL_LIB)
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(MUSL_CFLAGS) -T $(USERLAND_DIR)/user.ld -z noexecstack -o $@ \
		$(MUSL_DIR)/lib/crt1.o $(USERLAND_DIR)/musl/hello.c \
		-L$(MUSL_DIR)/lib -lc -lgcc


$(LIB_BUILD)/%.o: $(LIB_DIR)/%.c
	mkdir -p $(LIB_BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(LIB_BUILD)/crt0.o: $(LIB_DIR)/crt0.asm
	mkdir -p $(LIB_BUILD)
	$(AS) $(ASFLAGS) $(LIB_DIR)/crt0.asm -o $(LIB_BUILD)/crt0.o

$(LIB_BUILD)/libneoos.a: $(LIB_OBJECTS)
	ar rcs $(LIB_BUILD)/libneoos.a $(LIB_OBJECTS)

$(USERLAND_BUILD)/SPIN.ELF: $(USERLAND_DIR)/spin.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/spin.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/CHILD.ELF: $(USERLAND_DIR)/child.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/child.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/PARENT.ELF: $(USERLAND_DIR)/parent.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/parent.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/LOOPER.ELF: $(USERLAND_DIR)/looper.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/looper.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/YIELDER.ELF: $(USERLAND_DIR)/yielder.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/yielder.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/FAULTER.ELF: $(USERLAND_DIR)/faulter.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/faulter.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/FILEIO.ELF: $(USERLAND_DIR)/fileio.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/fileio.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/SSE_TEST.ELF: $(USERLAND_DIR)/sse_test.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/sse_test.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/FORKTEST.ELF: $(USERLAND_DIR)/fork_test.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/fork_test.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/EXECTARG.ELF: $(USERLAND_DIR)/exec_target.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/exec_target.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/MOUNTTST.ELF: $(USERLAND_DIR)/mounttest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/mounttest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/TTYTEST.ELF: $(USERLAND_DIR)/ttytest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/ttytest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/TIER0.ELF: $(USERLAND_DIR)/tier0test.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/tier0test.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/LFNTEST.ELF: $(USERLAND_DIR)/lfntest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/lfntest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/DIRTEST.ELF: $(USERLAND_DIR)/direnttest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/direnttest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/STATTEST.ELF: $(USERLAND_DIR)/stattest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/stattest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/CWDTEST.ELF: $(USERLAND_DIR)/cwdtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/cwdtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/VFSTEST.ELF: $(USERLAND_DIR)/vfstest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/vfstest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/VTTEST.ELF: $(USERLAND_DIR)/term/vttest.c $(USERLAND_DIR)/term/vt.c $(USERLAND_DIR)/term/vt.h $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -I$(USERLAND_DIR)/term -T $(USERLAND_DIR)/user.ld -o $@ \
	    $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/term/vttest.c $(USERLAND_DIR)/term/vt.c \
	    -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/ACTIVETTYTEST.ELF: $(USERLAND_DIR)/activettytest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/activettytest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/VTSWITCHTEST.ELF: $(USERLAND_DIR)/vtswitchtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/vtswitchtest.c -L$(LIB_BUILD) -lneoos

TERM_SRC := $(USERLAND_DIR)/term/main.c $(USERLAND_DIR)/term/vt.c $(USERLAND_DIR)/term/render.c $(USERLAND_DIR)/term/palette.c $(USERLAND_DIR)/term/font_term.c
TERM_HDR := $(USERLAND_DIR)/term/vt.h $(USERLAND_DIR)/term/render.h $(USERLAND_DIR)/term/palette.h $(USERLAND_DIR)/term/font_term.h

$(USERLAND_BUILD)/TERM.ELF: $(TERM_SRC) $(TERM_HDR) $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -I$(USERLAND_DIR)/term -T $(USERLAND_DIR)/user.ld -o $@ \
	    $(LIB_BUILD)/crt0.o $(TERM_SRC) -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/TERMCHILD.ELF: $(USERLAND_DIR)/termchild.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/termchild.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/THRDTEST.ELF: $(USERLAND_DIR)/threadtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/threadtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/SIGTEST.ELF: $(USERLAND_DIR)/sigtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/sigtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/AVXTEST.ELF: $(USERLAND_DIR)/avxtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -mavx -mavx2 -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/avxtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/SMPTEST.ELF: $(USERLAND_DIR)/smptest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/smptest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/EVTEST.ELF: $(USERLAND_DIR)/evtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/evtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/MMAPTEST.ELF: $(USERLAND_DIR)/mmaptest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/mmaptest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/REBTEST.ELF: $(USERLAND_DIR)/rebtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/rebtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/ORPHANTEST.ELF: $(USERLAND_DIR)/orphantest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/orphantest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/INIT.ELF: $(USERLAND_DIR)/init.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/init.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/FBTEST.ELF: $(USERLAND_DIR)/fbtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/fbtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/POLLTEST.ELF: $(USERLAND_DIR)/polltest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/polltest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/BBSH.ELF: $(USERLAND_DIR)/bbsh.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/bbsh.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/NSH.ELF: $(USERLAND_DIR)/nsh.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/nsh.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/NSHTEST.ELF: $(USERLAND_DIR)/nshtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/nshtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/LOGINTEST.ELF: $(USERLAND_DIR)/logintest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/logintest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/RANDTEST.ELF: $(USERLAND_DIR)/randtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/randtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/PERMTEST.ELF: $(USERLAND_DIR)/permtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/permtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/NEXCHECK.ELF: $(USERLAND_DIR)/nexcheck.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/nexcheck.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/BBSPIKE.ELF: $(USERLAND_DIR)/bbspike.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/bbspike.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/THRDMANY.ELF: $(USERLAND_DIR)/threadmany.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/threadmany.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/FDALLOC.ELF: $(USERLAND_DIR)/fdalloc.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/fdalloc.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/ARGVTEST.ELF: $(USERLAND_DIR)/argvtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/argvtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/POLLTRUNC.ELF: $(USERLAND_DIR)/polltrunc.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/polltrunc.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/TLBSTORM.ELF: $(USERLAND_DIR)/tlbstorm.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/tlbstorm.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/FORKSTORM.ELF: $(USERLAND_DIR)/forkstorm.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/forkstorm.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/PTYCHURN.ELF: $(USERLAND_DIR)/ptychurn.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/ptychurn.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/SIGSTORM.ELF: $(USERLAND_DIR)/sigstorm.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/sigstorm.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/POLLSTORM.ELF: $(USERLAND_DIR)/pollstorm.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/pollstorm.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/FAULTFLOOD.ELF: $(USERLAND_DIR)/faultflood.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/faultflood.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/PTYTEST.ELF: $(USERLAND_DIR)/ptytest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/ptytest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/IPCTEST.ELF: $(USERLAND_DIR)/ipctest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/ipctest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/PIPETEST.ELF: $(USERLAND_DIR)/pipetest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/pipetest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/TLSTEST.ELF: $(USERLAND_DIR)/tlstest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/tlstest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/NETTEST.ELF: $(USERLAND_DIR)/nettest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/nettest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/TCPTEST.ELF: $(USERLAND_DIR)/tcptest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/tcptest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/TCPWIRE.ELF: $(USERLAND_DIR)/tcpwire.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/tcpwire.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/MPITEST.ELF: $(USERLAND_DIR)/mpitest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/mpitest.c -L$(LIB_BUILD) -lneoos

$(DISK_IMG): $(USERLAND_BUILD)/SPIN.ELF $(USERLAND_BUILD)/CHILD.ELF $(USERLAND_BUILD)/PARENT.ELF $(USERLAND_BUILD)/LOOPER.ELF $(USERLAND_BUILD)/YIELDER.ELF $(USERLAND_BUILD)/FAULTER.ELF $(USERLAND_BUILD)/FILEIO.ELF $(USERLAND_BUILD)/SSE_TEST.ELF $(USERLAND_BUILD)/FORKTEST.ELF $(USERLAND_BUILD)/EXECTARG.ELF $(USERLAND_BUILD)/MOUNTTST.ELF $(USERLAND_BUILD)/VFSTEST.ELF $(USERLAND_BUILD)/VTTEST.ELF $(USERLAND_BUILD)/ACTIVETTYTEST.ELF $(USERLAND_BUILD)/VTSWITCHTEST.ELF $(USERLAND_BUILD)/TERM.ELF $(USERLAND_BUILD)/TERMCHILD.ELF $(USERLAND_BUILD)/THRDTEST.ELF $(USERLAND_BUILD)/SIGTEST.ELF $(USERLAND_BUILD)/AVXTEST.ELF $(USERLAND_BUILD)/MMAPTEST.ELF $(USERLAND_BUILD)/REBTEST.ELF $(USERLAND_BUILD)/ORPHANTEST.ELF $(USERLAND_BUILD)/INIT.ELF $(USERLAND_BUILD)/FBTEST.ELF $(USERLAND_BUILD)/POLLTEST.ELF $(USERLAND_BUILD)/POLLTRUNC.ELF $(USERLAND_BUILD)/ARGVTEST.ELF $(USERLAND_BUILD)/FDALLOC.ELF $(USERLAND_BUILD)/THRDMANY.ELF $(USERLAND_BUILD)/BBSPIKE.ELF $(USERLAND_BUILD)/NEXCHECK.ELF $(USERLAND_BUILD)/PERMTEST.ELF $(USERLAND_BUILD)/RANDTEST.ELF $(USERLAND_BUILD)/LOGINTEST.ELF $(USERLAND_BUILD)/NSH.ELF $(USERLAND_BUILD)/NSHTEST.ELF $(USERLAND_BUILD)/BBSH.ELF $(USERLAND_BUILD)/TLBSTORM.ELF $(USERLAND_BUILD)/FORKSTORM.ELF $(USERLAND_BUILD)/PTYCHURN.ELF $(USERLAND_BUILD)/SIGSTORM.ELF $(USERLAND_BUILD)/POLLSTORM.ELF $(USERLAND_BUILD)/FAULTFLOOD.ELF $(USERLAND_BUILD)/PTYTEST.ELF $(USERLAND_BUILD)/SMPTEST.ELF $(USERLAND_BUILD)/EVTEST.ELF $(USERLAND_BUILD)/IPCTEST.ELF $(USERLAND_BUILD)/PIPETEST.ELF $(USERLAND_BUILD)/TLSTEST.ELF $(USERLAND_BUILD)/NETTEST.ELF $(USERLAND_BUILD)/TCPTEST.ELF $(USERLAND_BUILD)/TCPWIRE.ELF $(USERLAND_BUILD)/MPITEST.ELF $(USERLAND_BUILD)/CWDTEST.ELF $(USERLAND_BUILD)/STATTEST.ELF $(USERLAND_BUILD)/DIRTEST.ELF $(USERLAND_BUILD)/LFNTEST.ELF $(USERLAND_BUILD)/TIER0.ELF $(USERLAND_BUILD)/MUSLHELO.ELF $(USERLAND_BUILD)/MUSLFORK.ELF $(USERLAND_BUILD)/LOGIN.ELF $(USERLAND_BUILD)/TTYTEST.ELF
	mkdir -p $(DISK_SRC)/dir $(DISK_SRC)/nex
	printf 'Hello from NeoOS FAT16!\n' > $(DISK_SRC)/hello.txt
	head -c 8192 /dev/zero | tr '\0' 'N' > $(DISK_SRC)/bigfile.txt
	printf 'nested file contents\n' > $(DISK_SRC)/dir/nested.txt
	printf 'a long name survived the round trip\n' > "$(DISK_SRC)/A Long File Name.txt"
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=32 status=none
	mkfs.fat -F 16 $(DISK_IMG)
	@# A clean root: directories only, no loose files. Everything below
	@# has a place -- programs in bin/sbin, the test suite in usr/tests,
	@# read-only fixtures in usr/share/test, and var/tmp for the tests
	@# that must write to a REAL filesystem (/tmp is a ramfs).
	mmd -i $(DISK_IMG) ::bin
	mmd -i $(DISK_IMG) ::sbin
	mmd -i $(DISK_IMG) ::etc
	mmd -i $(DISK_IMG) ::usr
	mmd -i $(DISK_IMG) ::usr/tests
	mmd -i $(DISK_IMG) ::usr/share
	mmd -i $(DISK_IMG) ::usr/share/test
	mmd -i $(DISK_IMG) ::usr/share/test/dir
	mmd -i $(DISK_IMG) ::var
	mmd -i $(DISK_IMG) ::var/tmp
	mmd -i $(DISK_IMG) ::home
	mmd -i $(DISK_IMG) ::root
	mcopy -i $(DISK_IMG) $(DISK_SRC)/hello.txt ::usr/share/test/hello.txt
	mcopy -i $(DISK_IMG) "$(DISK_SRC)/A Long File Name.txt" "::usr/share/test/A Long File Name.txt"
	mcopy -i $(DISK_IMG) $(DISK_SRC)/bigfile.txt ::usr/share/test/bigfile.txt
	mcopy -i $(DISK_IMG) $(DISK_SRC)/dir/nested.txt ::usr/share/test/dir/nested.txt
	@./tools/nexify.sh $(USERLAND_BUILD)/SPIN.ELF $(DISK_SRC)/nex/spin.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/spin.nex ::usr/tests/spin.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/CHILD.ELF $(DISK_SRC)/nex/child.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/child.nex ::usr/tests/child.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/PARENT.ELF $(DISK_SRC)/nex/parent.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/parent.nex ::usr/tests/parent.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/LOOPER.ELF $(DISK_SRC)/nex/looper.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/looper.nex ::usr/tests/looper.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/YIELDER.ELF $(DISK_SRC)/nex/yielder.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/yielder.nex ::usr/tests/yielder.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/FAULTER.ELF $(DISK_SRC)/nex/faulter.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/faulter.nex ::usr/tests/faulter.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/FILEIO.ELF $(DISK_SRC)/nex/fileio.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/fileio.nex ::usr/tests/fileio.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/SSE_TEST.ELF $(DISK_SRC)/nex/sse_test.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/sse_test.nex ::usr/tests/sse_test.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/FORKTEST.ELF $(DISK_SRC)/nex/forktest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/forktest.nex ::usr/tests/forktest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/EXECTARG.ELF $(DISK_SRC)/nex/exectarg.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/exectarg.nex ::usr/tests/exectarg.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/MOUNTTST.ELF $(DISK_SRC)/nex/mounttst.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/mounttst.nex ::usr/tests/mounttst.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/VFSTEST.ELF $(DISK_SRC)/nex/vfstest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/vfstest.nex ::usr/tests/vfstest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/VTTEST.ELF $(DISK_SRC)/nex/vttest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/vttest.nex ::usr/tests/vttest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/ACTIVETTYTEST.ELF $(DISK_SRC)/nex/activettytest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/activettytest.nex ::usr/tests/activettytest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/VTSWITCHTEST.ELF $(DISK_SRC)/nex/vtswitchtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/vtswitchtest.nex ::usr/tests/vtswitchtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/TERM.ELF $(DISK_SRC)/nex/term.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/term.nex ::bin/term.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/TERMCHILD.ELF $(DISK_SRC)/nex/termchild.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/termchild.nex ::usr/tests/termchild.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/THRDTEST.ELF $(DISK_SRC)/nex/thrdtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/thrdtest.nex ::usr/tests/thrdtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/SIGTEST.ELF $(DISK_SRC)/nex/sigtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/sigtest.nex ::usr/tests/sigtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/AVXTEST.ELF $(DISK_SRC)/nex/avxtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/avxtest.nex ::usr/tests/avxtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/MMAPTEST.ELF $(DISK_SRC)/nex/mmaptest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/mmaptest.nex ::usr/tests/mmaptest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/REBTEST.ELF $(DISK_SRC)/nex/rebtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/rebtest.nex ::usr/tests/rebtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/ORPHANTEST.ELF $(DISK_SRC)/nex/orphantest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/orphantest.nex ::usr/tests/orphantest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/FBTEST.ELF $(DISK_SRC)/nex/fbtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/fbtest.nex ::usr/tests/fbtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/POLLTEST.ELF $(DISK_SRC)/nex/polltest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/polltest.nex ::usr/tests/polltest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/POLLTRUNC.ELF $(DISK_SRC)/nex/polltrunc.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/polltrunc.nex ::usr/tests/polltrunc.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/ARGVTEST.ELF $(DISK_SRC)/nex/argvtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/argvtest.nex ::usr/tests/argvtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/FDALLOC.ELF $(DISK_SRC)/nex/fdalloc.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/fdalloc.nex ::usr/tests/fdalloc.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/THRDMANY.ELF $(DISK_SRC)/nex/thrdmany.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/thrdmany.nex ::usr/tests/thrdmany.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/NSH.ELF $(DISK_SRC)/nex/nsh.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/nsh.nex ::bin/nsh.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/NSHTEST.ELF $(DISK_SRC)/nex/nshtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/nshtest.nex ::usr/tests/nshtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/LOGINTEST.ELF $(DISK_SRC)/nex/logintest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/logintest.nex ::usr/tests/logintest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/LOGIN.ELF $(DISK_SRC)/nex/login.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/login.nex ::sbin/login.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/RANDTEST.ELF $(DISK_SRC)/nex/randtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/randtest.nex ::usr/tests/randtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/PERMTEST.ELF $(DISK_SRC)/nex/permtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/permtest.nex ::usr/tests/permtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/NEXCHECK.ELF $(DISK_SRC)/nex/nexcheck.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/nexcheck.nex ::usr/tests/nexcheck.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/BBSPIKE.ELF $(DISK_SRC)/nex/bbspike.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/bbspike.nex ::usr/tests/bbspike.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/BBSH.ELF $(DISK_SRC)/nex/bbsh.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/bbsh.nex ::usr/tests/bbsh.nex
	@# BusyBox is optional: `make busybox` builds it, and the image
	@# picks it up if it is present. A tree that has never built it
	@# still produces a bootable disk.
	@# Ports are optional on the image, like busybox: `make ports` builds
	@# them and the image picks them up if they are there, so a tree that
	@# has never built one still produces a bootable disk.
	@if [ -f $(AV_BIN) ]; then \
	    ./tools/nexify.sh $(AV_BIN) $(DISK_SRC)/nex/av.nex && \
	    mcopy -o -i $(DISK_IMG) $(DISK_SRC)/nex/av.nex ::bin/av.nex && \
	    mmd -i $(DISK_IMG) ::usr/share/models 2>/dev/null; \
	    for f in $(AV_DIR)/upstream/models/*.obj $(AV_DIR)/upstream/models/*.mtl; do \
	      mcopy -o -i $(DISK_IMG) $$f ::usr/share/models/$$(basename $$f); \
	    done; \
	    echo "disk: 3d-ascii-viewer as /bin/av.nex, $$(ls $(AV_DIR)/upstream/models/*.obj | wc -l) models in /usr/share/models"; \
	else \
	    echo "disk: no ports (run 'make ports')"; \
	fi
	@if [ -f $(BUSYBOX_BIN) ]; then \
	    ./tools/nexify.sh $(BUSYBOX_BIN) $(DISK_SRC)/nex/busybox.nex && \
	    mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/busybox.nex ::bin/busybox.nex && \
	    echo "disk: BusyBox included ($$(stat -c%s $(BUSYBOX_BIN)) bytes)"; \
	else \
	    echo "disk: no BusyBox (run 'make busybox')"; \
	fi
	@./tools/nexify.sh $(USERLAND_BUILD)/TLBSTORM.ELF $(DISK_SRC)/nex/tlbstorm.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/tlbstorm.nex ::usr/tests/tlbstorm.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/FORKSTORM.ELF $(DISK_SRC)/nex/forkstorm.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/forkstorm.nex ::usr/tests/forkstorm.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/PTYCHURN.ELF $(DISK_SRC)/nex/ptychurn.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/ptychurn.nex ::usr/tests/ptychurn.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/SIGSTORM.ELF $(DISK_SRC)/nex/sigstorm.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/sigstorm.nex ::usr/tests/sigstorm.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/POLLSTORM.ELF $(DISK_SRC)/nex/pollstorm.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/pollstorm.nex ::usr/tests/pollstorm.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/FAULTFLOOD.ELF $(DISK_SRC)/nex/faultflood.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/faultflood.nex ::usr/tests/faultflood.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/PTYTEST.ELF $(DISK_SRC)/nex/ptytest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/ptytest.nex ::usr/tests/ptytest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/SMPTEST.ELF $(DISK_SRC)/nex/smptest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/smptest.nex ::usr/tests/smptest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/EVTEST.ELF $(DISK_SRC)/nex/evtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/evtest.nex ::usr/tests/evtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/IPCTEST.ELF $(DISK_SRC)/nex/ipctest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/ipctest.nex ::usr/tests/ipctest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/PIPETEST.ELF $(DISK_SRC)/nex/pipetest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/pipetest.nex ::usr/tests/pipetest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/TLSTEST.ELF $(DISK_SRC)/nex/tlstest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/tlstest.nex ::usr/tests/tlstest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/NETTEST.ELF $(DISK_SRC)/nex/nettest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/nettest.nex ::usr/tests/nettest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/TCPTEST.ELF $(DISK_SRC)/nex/tcptest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/tcptest.nex ::usr/tests/tcptest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/TCPWIRE.ELF $(DISK_SRC)/nex/tcpwire.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/tcpwire.nex ::usr/tests/tcpwire.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/MPITEST.ELF $(DISK_SRC)/nex/mpitest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/mpitest.nex ::usr/tests/mpitest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/CWDTEST.ELF $(DISK_SRC)/nex/cwdtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/cwdtest.nex ::usr/tests/cwdtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/STATTEST.ELF $(DISK_SRC)/nex/stattest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/stattest.nex ::usr/tests/stattest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/DIRTEST.ELF $(DISK_SRC)/nex/dirtest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/dirtest.nex ::usr/tests/dirtest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/LFNTEST.ELF $(DISK_SRC)/nex/lfntest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/lfntest.nex ::usr/tests/lfntest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/TIER0.ELF $(DISK_SRC)/nex/tier0.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/tier0.nex ::usr/tests/tier0.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/MUSLHELO.ELF $(DISK_SRC)/nex/muslhelo.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/muslhelo.nex ::usr/tests/muslhelo.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/MUSLFORK.ELF $(DISK_SRC)/nex/muslfork.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/muslfork.nex ::usr/tests/muslfork.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/TTYTEST.ELF $(DISK_SRC)/nex/ttytest.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/ttytest.nex ::usr/tests/ttytest.nex
	@./tools/nexify.sh $(USERLAND_BUILD)/INIT.ELF $(DISK_SRC)/nex/init.nex
	@mcopy -i $(DISK_IMG) $(DISK_SRC)/nex/init.nex ::sbin/init.nex
	printf '%s\n' \
	  '# NeoOS test workload -- launched by /sbin/init.nex (see userland/init.c)' \
	  '# TERM runs first as a wait entry: it claims the active tty +' \
	  '# framebuffer, renders TERMCHILD, self-checks, and exits before the' \
	  '# key-injecting suites (ttytest, evtest, polltest) start.' \
	  'wait /bin/term.nex' \
	  '# VTSWITCHTEST is a wait entry for the same reason: the active VT' \
	  '# is global state, and /dev/CONSOLE follows it. A concurrent' \
	  '# ttytest that set ICANON off through /dev/CONSOLE and read it' \
	  '# back after a switch got two DIFFERENT terminals, and reported' \
	  '# that the setting had not stuck.' \
	  'wait /usr/tests/vtswitchtest.nex' \
	  'spawn /usr/tests/parent.nex' \
	  'spawn /usr/tests/looper.nex' \
	  'spawn /usr/tests/looper.nex' \
	  'spawn /usr/tests/yielder.nex' \
	  'spawn /usr/tests/vfstest.nex' \
	  'spawn /usr/tests/vttest.nex' \
	  'spawn /usr/tests/activettytest.nex' \
	  'spawn /usr/tests/cwdtest.nex' \
	  'spawn /usr/tests/stattest.nex' \
	  'spawn /usr/tests/dirtest.nex' \
	  'spawn /usr/tests/lfntest.nex' \
	  'spawn /usr/tests/tier0.nex' \
	  'spawn /usr/tests/muslhelo.nex' \
	  'spawn /usr/tests/muslfork.nex' \
	  'spawn /usr/tests/ttytest.nex' \
	  'spawn /usr/tests/thrdtest.nex' \
	  'spawn /usr/tests/sigtest.nex' \
	  'spawn /usr/tests/faulter.nex' \
	  'spawn /usr/tests/avxtest.nex' \
	  'spawn /usr/tests/mmaptest.nex' \
	  'spawn /usr/tests/rebtest.nex' \
	  'spawn /usr/tests/orphantest.nex' \
	  'spawn /usr/tests/fbtest.nex' \
	  'spawn /usr/tests/polltest.nex' \
	  'spawn /usr/tests/polltrunc.nex' \
	  'spawn /usr/tests/argvtest.nex' \
	  'spawn /usr/tests/nexcheck.nex' \
	  'spawn /usr/tests/permtest.nex' \
	  'spawn /usr/tests/randtest.nex' \
	  'spawn /usr/tests/fdalloc.nex' \
	  'spawn /usr/tests/thrdmany.nex' \
	  '# BBSPIKE is a wait entry: it is a survey, and its value is a' \
	  '# readable transcript of what BusyBox did. Interleaved with the' \
	  '# rest of the suite that transcript is unreadable.' \
	  'wait /usr/tests/bbspike.nex' \
	  '# BBSH runs an interactive shell on a pty and claims the active' \
	  '# tty for it. A wait entry for the same reason PTYCHURN is one.' \
	  'wait /usr/tests/nshtest.nex' \
	  'wait /usr/tests/logintest.nex' \
	  'wait /usr/tests/bbsh.nex' \
	  'spawn /usr/tests/tlbstorm.nex' \
	  'spawn /usr/tests/forkstorm.nex' \
	  '# PTYCHURN is a wait entry: it deliberately holds the ENTIRE pty' \
	  '# pool (all 16) for the length of a round, so anything opening a' \
	  '# pty beside it gets -ENFILE. ptytest did exactly that.' \
	  'wait /usr/tests/ptychurn.nex' \
	  'spawn /usr/tests/sigstorm.nex' \
	  '# POLLSTORM is a wait entry: it measures poll-broadcast traffic in' \
	  '# two windows and compares them, so the background load has to be' \
	  '# the same in both. Run beside the rest of the suite it swung 4.5x.' \
	  'wait /usr/tests/pollstorm.nex' \
	  'spawn /usr/tests/ptytest.nex' \
	  'spawn /usr/tests/smptest.nex' \
	  'spawn /usr/tests/evtest.nex' \
	  'spawn /usr/tests/ipctest.nex' \
	  'spawn /usr/tests/pipetest.nex' \
	  'spawn /usr/tests/tlstest.nex' \
	  'spawn /usr/tests/nettest.nex' \
	  'spawn /usr/tests/tcptest.nex' \
	  'spawn /usr/tests/tcpwire.nex' \
	  'spawn /usr/tests/mpitest.nex' \
	  > $(DISK_SRC)/inittab
	mcopy -i $(DISK_IMG) $(DISK_SRC)/inittab ::etc/inittab
	@# The shell's greeting, generated from the SAME art the kernel
	@# banner draws (shared/neoos_logo.h) so the two cannot drift.
	@python3 tools/logo2ansi.py shared/neoos_logo.h > $(DISK_SRC)/nsh.logo
	@mcopy -o -i $(DISK_IMG) $(DISK_SRC)/nsh.logo ::etc/nsh.logo
	@printf '%s\n' \
	  '# /etc/nshrc -- read by nsh before its first prompt.' \
	  '#' \
	  '# Every non-blank, non-comment line is a command nsh runs. That is' \
	  '# the whole format: "a list of commands" already covers showing a' \
	  '# banner or changing directory, and a second syntax would be a' \
	  '# second thing to learn.' \
	  '' \
	  'cat /etc/nsh.logo' \
	  > $(DISK_SRC)/nshrc
	@mcopy -o -i $(DISK_IMG) $(DISK_SRC)/nshrc ::etc/nshrc
	@# /etc/passwd, with REAL hashes: $$6$$ is SHA-512-crypt, a
	@# rounds-based KDF that musl's crypt() verifies on the other side.
	@# Generated here rather than checked in, so the file and the
	@# passwords documented beside it cannot drift apart.
	@printf '%s\n' \
	  '# name:uid:gid:home:shell:hash   -- see docs/stdlib.md' \
	  "god:0:0:/root:/bin/nsh.nex:$$(openssl passwd -6 -salt neoosgod god)" \
	  "neo:1000:1000:/home/neo:/bin/nsh.nex:$$(openssl passwd -6 -salt neoosusr neo)" \
	  > $(DISK_SRC)/passwd
	@mcopy -o -i $(DISK_IMG) $(DISK_SRC)/passwd ::etc/passwd
	@mmd -i $(DISK_IMG) ::home/neo 2>/dev/null || true

# 64MB, not 32: FAT32 needs at least 65525 clusters, and mkfs.fat warns
# that a 32MB image falls below that. Depends on $(DISK_IMG) only so
# that rule's mkdir of $(DISK_SRC) has already run.
$(DISK2_IMG): $(DISK_IMG)
	dd if=/dev/zero of=$(DISK2_IMG) bs=1M count=64 status=none
	mkfs.fat -F 32 $(DISK2_IMG)
	printf 'Hello from the FAT32 volume!\n' > $(DISK_SRC)/fat32.txt
	mcopy -i $(DISK2_IMG) $(DISK_SRC)/fat32.txt ::fat32.txt
	mmd -i $(DISK2_IMG) ::sub
	printf 'nested on fat32\n' > $(DISK_SRC)/f32nest.txt
	mcopy -i $(DISK2_IMG) $(DISK_SRC)/f32nest.txt ::sub/f32nest.txt

disk-image: $(DISK_IMG) $(DISK2_IMG)

SMP_CPUS ?= 4
# The reference machine is TCG with -cpu Nehalem (CLAUDE.md): it is what
# the gauntlet's flake signatures are tuned for, and it pins the CPU
# feature set the tests actually cover. KVM=1 swaps in hardware
# virtualisation for local iteration -- 2.3x faster on a full boot, and
# it gives TRUE parallelism where TCG round-robins the vCPUs, so it can
# reach SMP interleavings TCG cannot. Use it to iterate; sign off on the
# default.
ifdef KVM
QEMU_MACHINE := -enable-kvm -cpu host
else
QEMU_MACHINE := -cpu Nehalem
endif

# D1. The NIC is virtio-net, not the e1000 QEMU attaches by default --
# see docs/superpowers/specs/2026-09-03-d1-virtio-net-design.md for why
# the paravirtual device was chosen over the more realistic one.
# Naming any -netdev/-device also suppresses QEMU's default NIC, so this
# machine has exactly one interface and pci_selftest keeps seeing the
# bus it asserts on.
#
# `user` (SLIRP) is the default because it needs NO PRIVILEGES: it
# answers ARP for the gateway at 10.0.2.2, which is what
# virtio_net_selftest proves the driver against, and it works in CI and
# under the gauntlet with no host setup at all. The host cannot ping
# INTO it -- that is what `make shell-tap` is for.
QEMU_NETDEV ?= user,id=net0
QEMU_NET := -netdev $(QEMU_NETDEV) -device virtio-net-pci,netdev=net0

QEMU_COMMON := $(QEMU_MACHINE) -smp $(SMP_CPUS) -boot order=d \
	-cdrom $(BUILD_DIR)/neoos.iso \
	-drive file=$(DISK_IMG),format=raw -drive file=$(DISK2_IMG),format=raw \
	-vga std \
	$(QEMU_NET) \
	-no-reboot

# -vga std: the plain Bochs-VBE standard VGA -- a dumb linear
# framebuffer, no acceleration, no guest driver. It is QEMU's current
# default on `pc`, but naming it keeps GRUB's Multiboot2 framebuffer
# request (and so /dev/fb0) working if that default ever changes.

run: iso disk-image
	qemu-system-x86_64 $(QEMU_COMMON)

# Boot straight to an interactive BusyBox ash on the framebuffer
# terminal, with nothing else running. Type in the QEMU window.
#
# The INITTAB is a `wait` entry for the terminal and nothing else: the
# whole test suite would otherwise be printing over the shell, and the
# machine powers off when the last user process exits -- so if the
# terminal is the only entry, leaving the shell ends the session.
#
# `respawn` rather than `spawn` would give a login-like loop, but a
# shell that exits should end the machine here, not restart forever.
shell:
	@$(MAKE) QUIET=1 clean-kernel busybox iso disk-image
	@printf '%s\n' \
	  '# generated by `make shell` -- an interactive shell, alone' \
	  'respawn /bin/term.nex /sbin/login.nex login' \
	  > $(BUILD_DIR)/disk-src/INITTAB.shell
	@mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/disk-src/INITTAB.shell ::etc/inittab
	@echo "NeoOS: booting to a login prompt."
	@echo "       Accounts: god/god (uid 0) and neo/neo (uid 1000)."
	@echo "       Leaving the shell returns a fresh login, so the machine stays up;"
	@echo "       close the window to stop it. Serial log: $(BUILD_DIR)/shell.log"
	@echo "       Type in the QEMU window. 'exit' starts a FRESH shell"
	@echo "       (respawn), so the machine stays up; close the window to"
	@echo "       stop it. Serial log: $(BUILD_DIR)/shell.log"
	qemu-system-x86_64 $(QEMU_COMMON) -serial file:$(BUILD_DIR)/shell.log

# The same shell, on a TAP interface instead of SLIRP: the host and
# NeoOS are then two machines on one wire, so the host can ping NeoOS
# and act as a real TCP peer -- neither of which SLIRP allows.
#
# It needs ONE-TIME HOST SETUP, as root, which is exactly why it is
# never on the automated path:
#
#   sudo ip tuntap add dev tap0 mode tap user $$USER
#   sudo ip addr add 10.0.2.2/24 dev tap0
#   sudo ip link set tap0 up
#
# NeoOS is then 10.0.2.15 and the host is 10.0.2.2 -- the same addresses
# SLIRP hands out, so nothing in the kernel has to know which of the two
# it is running on.
.PHONY: shell-tap
shell-tap:
	@$(MAKE) QEMU_NETDEV='tap,id=net0,ifname=tap0,script=no,downscript=no' shell

# The same shell, but driven over the serial port instead of the
# framebuffer -- useful when the QEMU window is not available (ssh, a
# headless host). stdin/stdout of the shell are the emulator's terminal.
shell-serial:
	@$(MAKE) QUIET=1 clean-kernel busybox iso disk-image
	@printf '%s\n' \
	  '# generated by `make shell-serial`' \
	  'respawn /bin/term.nex /sbin/login.nex login' \
	  > $(BUILD_DIR)/disk-src/INITTAB.shell
	@mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/disk-src/INITTAB.shell ::etc/inittab
	@echo "NeoOS: booting to a login prompt on the framebuffer; serial is the log."
	qemu-system-x86_64 $(QEMU_COMMON) -serial stdio

# Headless boot with COM1 captured to a file. Fails if any selftest
# reported FAILED, or if the boot never reached the marker (a hang or a
# triple fault).
#
# The kernel powers off via ACPI (outw 0x2000 -> 0x604) as soon as the
# last user process exits (kernel/sched/proc.c:user_proc_exited), so a
# healthy run exits QEMU on its own. BOOT_TIMEOUT is the HANG detector,
# not the normal exit path -- if it fires, something deadlocked. The
# qemu line keeps its leading `-` so a genuine timeout is still surfaced
# by the marker check rather than aborting the recipe.
#
# 150s, and it went to 240 and back again. The raise was made from
# CONTAMINATED evidence: two gauntlets were running against the same
# build tree at once, starving each other, and their timeouts were read
# as the suite having grown. Measured properly from the tick log, a
# healthy boot is FORTY-EIGHT SECONDS and tcptest costs about one of
# them.
#
# So 150 stands, and it stands as a HANG DETECTOR: three times a healthy
# boot. Raising it would only have made a real hang -- and there is one,
# see docs/superpowers/specs/2026-09-04-...-design.md -- take longer to
# report itself.
BOOT_TIMEOUT ?= 150
BOOT_MARKER  ?= NeoOS: interrupts enabled, starting scheduler

# A suite that never RUNS is not a pass. Grepping only for FAILED lets a
# silently-missing suite look identical to a green one -- which it did,
# until a scheduler bug stopped three of the five from ever reporting and
# `make test` still said PASS. grep -F because "[vfstest]" is a character
# class to a regex, and would never match the literal brackets.
REQUIRED_MARKERS := \
	"[pci] ALL PASSED" \
	"[virtio-net] ALL PASSED" \
	"[route] ALL PASSED" \
	"[arp] ALL PASSED" \
	"[icmp] ALL PASSED" \
	"[dhcp] ALL PASSED" \
	"[dns] ALL PASSED" \
	"[tcp] ALL PASSED" \
	"[smp] local timer selftest passed" \
	"[smp] steal selftest passed" \
	"[vfstest] ALL PASSED" \
	"[vttest] ALL PASSED" \
	"[activetty] ALL PASSED" \
	"[vtswitchtest] ALL PASSED" \
	"[term] render ALL PASSED" \
	"[avxtest] ALL PASSED" \
	"[mmaptest] ALL PASSED" \
	"[rebtest] ALL PASSED" \
	"[orphantest] ALL PASSED" \
	"[init] all entries exited -- powering off" \
	"[fbtest] ALL PASSED" \
	"[polltest] ALL PASSED" \
	"[polltrunc] ALL PASSED" \
	"[argvtest] ALL PASSED" \
	"[nexcheck] ALL PASSED" \
	"[permtest] ALL PASSED" \
	"[randtest] ALL PASSED" \
	"[nshtest] ALL PASSED" \
	"[logintest] ALL PASSED" \
	"[fdalloc] ALL PASSED" \
	"[threadmany] ALL PASSED" \
	"[bbspike] ALL PASSED" \
	"[bbsh] ALL PASSED" \
	"[tlbstorm] ALL PASSED" \
	"[forkstorm] ALL PASSED" \
	"[ptychurn] ALL PASSED" \
	"[sigstorm] ALL PASSED" \
	"[pollstorm] ALL PASSED" \
	"[ptytest] ALL PASSED" \
	"[devfs] selftest passed" \
	"[threadtest] ALL PASSED" \
	"[sigtest] ALL PASSED" \
	"[smptest] ALL PASSED" \
	"[ipctest] ALL PASSED" \
	"[pipetest] ALL PASSED" \
	"[tlstest] ALL PASSED" \
	"[nettest] ALL PASSED" \
	"[tcptest] ALL PASSED" \
	"[mpitest] ALL PASSED" \
	"[cwdtest] ALL PASSED" \
	"[stattest] ALL PASSED" \
	"[direnttest] ALL PASSED" \
	"[lfntest] ALL PASSED" \
	"[tier0test] ALL PASSED" \
	"[musltest] ALL PASSED" \
	"[ttytest] ALL PASSED" \
	"[evtest] ALL PASSED" \
	"[tty] selftest passed" \
	"[wxorx] kernel selftest passed" \
	"[fb] framebuffer" \
	"[fbdev] selftest passed" \
	"[fbcon] selftest passed" \
	"[con] selftest passed" \
	"[kvt] selftest passed" \
	"[vt] selftest passed" \
	"[banner]" \
	"[rtc] selftest passed" \
	"[keyboard] decode selftest passed" \
	"[input] selftest passed"

# The fat16 WRITE selftest creates /NEWDIR and /RT.TXT on the real disk
# image, and the image persists between runs -- so on the second boot
# mkdir(/NEWDIR) fails with "already exists" and every later `make test`
# reports a failure that has nothing to do with the change under test.
# Rebuilding the images makes each test run start from a known state.

clean-kernel:
	mkdir -p $(BUILD_DIR) && find $(BUILD_DIR) -name '*.o' -delete

# ---- terminal font -------------------------------------------------
#
# Spleen 12x24 (third_party/spleen, BSD-2-Clause) -> a packed C glyph
# table for the M1b userland terminal. The generated font_term.c/.h are
# CHECKED IN (like kernel/dev/font8x16.c), so a normal build never runs
# python. `font-regen` rewrites them; `font-check` proves the committed
# copy still matches the BDF and that the table compiles and indexes.
FONT_BDF := third_party/spleen/spleen-12x24.bdf
FONT_HDR := userland/term/font_term.h
FONT_SRC := userland/term/font_term.c

font-regen:
	python3 tools/bdf2c.py $(FONT_BDF) --array term_glyphs \
	    --header $(FONT_HDR) --source $(FONT_SRC)

# CS5.3 / CS5.4. Host-side, so it runs without a boot: duplicate lock
# ranks and undocumented spin_lock_raw sites are both invisible to the
# kernel's own runtime checker -- a collision makes an inversion LEGAL,
# and a raw acquire skips the check entirely.
lock-check:
	@python3 tools/lock_check.py

font-check:
	@tmp=$$(mktemp -d); rc=0; \
	python3 tools/bdf2c.py $(FONT_BDF) --array term_glyphs \
	    --header $$tmp/font_term.h --source $$tmp/font_term.c; \
	diff -u $(FONT_HDR) $$tmp/font_term.h || rc=1; \
	diff -u $(FONT_SRC) $$tmp/font_term.c || rc=1; \
	if [ $$rc -eq 0 ]; then \
	    echo "font-check: generated table matches the committed one"; \
	    cc -std=c11 -Wall -Wextra -o $$tmp/font_check tools/font_check.c && \
	    $$tmp/font_check || rc=1; \
	fi; \
	rm -rf $$tmp; exit $$rc

# Host check for the terminal glyph blitter (userland/term/render.c).
# Not part of `make test` -- it is userland tooling; the on-hardware
# proof is M1b-3's [term] render self-check.
render-check:
	cc -std=c11 -Wall -Wextra -o /tmp/neoos-rendertest tools/rendertest.c
	/tmp/neoos-rendertest

fresh-disks:
	rm -f $(DISK_IMG) $(DISK2_IMG)

# An EMPTY serial log means QEMU never got as far as running the kernel,
# and that is not a boot failure. Reporting it as one ("BOOT DID NOT
# COMPLETE", followed by thirty lines of nothing) sent a real debugging
# session chasing an early-boot race that did not exist.
#
# The usual cause is a lingering qemu -- from a `make test` that was
# interrupted, or one running in another terminal -- still holding the
# write lock on the disk images. qemu TRUNCATES the serial file first
# and only then fails to open the drive, so the log is zero bytes and
# looks exactly like a triple fault before serial_init. Its stderr says
# precisely what went wrong ("Failed to get \"write\" lock"), which is
# why it goes to a file now instead of /dev/null.
#
# Worth knowing when hunting a lingering qemu: the kernel never powers
# the machine off, so EVERY `make test` runs the full BOOT_TIMEOUT and
# is killed by `timeout`. Interrupting one is the easy way to leave a
# qemu behind.
# lock-check runs FIRST and is host-side: it costs a second and catches
# two classes of mistake a boot cannot, so there is no reason to spend a
# 150-second boot before hearing about them.
test: lock-check clean-kernel fresh-disks iso disk-image
	@mkdir -p $(BUILD_DIR)
	-@timeout $(BOOT_TIMEOUT) qemu-system-x86_64 $(QEMU_COMMON) \
		-display gtk -serial file:$(BUILD_DIR)/serial.log \
		> /dev/null 2>$(BUILD_DIR)/qemu.err
	@echo "--- serial log: $(BUILD_DIR)/serial.log ---"
	@if [ ! -s $(BUILD_DIR)/serial.log ]; then \
		echo "QEMU PRODUCED NO OUTPUT AT ALL -- it likely never started."; \
		echo "--- qemu stderr ---"; cat $(BUILD_DIR)/qemu.err; \
		echo "--- still-running qemu processes ---"; \
		pgrep -a qemu-system-x86_64 || echo "(none)"; \
		exit 1; fi
	@if grep -q 'PANIC' $(BUILD_DIR)/serial.log; then \
		echo "KERNEL PANIC:"; grep 'PANIC' $(BUILD_DIR)/serial.log; exit 1; fi
	@if grep -q 'FAILED' $(BUILD_DIR)/serial.log; then \
		echo "TEST FAILURES:"; grep 'FAILED' $(BUILD_DIR)/serial.log; exit 1; fi
	@if ! grep -q '$(BOOT_MARKER)' $(BUILD_DIR)/serial.log; then \
		echo "BOOT DID NOT COMPLETE (no marker: '$(BOOT_MARKER)')"; \
		tail -30 $(BUILD_DIR)/serial.log; exit 1; fi
	@for m in $(REQUIRED_MARKERS); do \
		if ! grep -qF "$$m" $(BUILD_DIR)/serial.log; then \
			echo "MISSING EXPECTED RESULT: $$m"; exit 1; fi; done
	@echo "PASS: no FAILED lines, boot reached the scheduler, all suites reported"

# The WIRE test, which needs a host-side helper and therefore cannot be
# part of `make test`.
#
# tcpwire connects OUT to 10.0.2.2, because QEMU's user-mode networking
# forwards guest-initiated TCP to the host with no privileges and no tap
# device -- so this runs in CI as an ordinary user. Without a server it
# reports SKIPPED (the connect is REFUSED, which already proves the
# stack reached the host), which is why `[tcpwire] ALL PASSED` is not in
# REQUIRED_MARKERS: a developer with no Python must still get a green
# `make test`. This target is where the marker is actually demanded.
.PHONY: test-wire
TCPWIRE_PORT ?= 7900
test-wire: iso disk-image
	@python3 tools/tcp-echo-server.py $(TCPWIRE_PORT) > $(BUILD_DIR)/echo.log 2>&1 & 	 echo $$! > $(BUILD_DIR)/echo.pid
	@sleep 1
	-@timeout $(BOOT_TIMEOUT) qemu-system-x86_64 $(QEMU_COMMON) 		-display none -serial file:$(BUILD_DIR)/wire.log > /dev/null 2>&1
	-@kill `cat $(BUILD_DIR)/echo.pid` 2>/dev/null || true
	@grep -E '^\[tcpwire\]' $(BUILD_DIR)/wire.log || true
	@cat $(BUILD_DIR)/echo.log
	@if ! grep -qF "[tcpwire] ALL PASSED" $(BUILD_DIR)/wire.log; then 		echo "TEST-WIRE FAILED: no [tcpwire] ALL PASSED marker"; exit 1; fi
	@echo "TEST-WIRE PASSED: a TCP connection left the machine and came back"

# faultflood needs the machine to ITSELF: it deliberately drives the
# system out of frames, and a `wait` entry does not stop tests already
# spawned, so in the shared suite it starved whatever was still running
# (forkstorm watched free frames go 29377 -> 319). It gets its own boot
# with an INITTAB containing nothing else.
faultflood: iso disk-image
	@printf '%s\n' \
	  '# generated by `make faultflood` -- this test runs alone' \
	  'wait /usr/tests/faultflood.nex' \
	  > $(BUILD_DIR)/disk-src/INITTAB.solo
	mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/disk-src/INITTAB.solo ::etc/inittab
	-@timeout $(BOOT_TIMEOUT) qemu-system-x86_64 $(QEMU_COMMON) \
		-display none -serial file:$(BUILD_DIR)/faultflood.log > /dev/null 2>&1
	@if grep -qE 'PANIC|\[exception\]' $(BUILD_DIR)/faultflood.log; then \
		echo "FAULTFLOOD: the KERNEL did not survive"; \
		grep -E 'PANIC|\[exception\]' $(BUILD_DIR)/faultflood.log; exit 1; fi
	@if ! grep -q '\[faultflood\] ALL PASSED' $(BUILD_DIR)/faultflood.log; then \
		echo "FAULTFLOOD: no ALL PASSED marker"; tail -5 $(BUILD_DIR)/faultflood.log; exit 1; fi
	@grep -E '^\[faultflood\]' $(BUILD_DIR)/faultflood.log
	@echo "FAULTFLOOD PASSED: kernel survived running out of frames"

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR)
