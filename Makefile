CC := $(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-gcc
AS := nasm

NEOOS_GITREV := $(shell git describe --always --dirty --tags 2>/dev/null || echo unknown)
CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -Wall -Wextra -std=gnu11 -O2 -Ikernel -DNEOOS_GITREV='"$(NEOOS_GITREV)"'
ASFLAGS := -f elf64

ifdef NEOOS_DEBUG_STOP_WINDOW
CFLAGS += -DNEOOS_DEBUG_STOP_WINDOW
endif

ifdef DEBUG_HEAP
CFLAGS += -DNEOOS_DEBUG_HEAP
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
	kernel/tty kernel/ipc kernel/smp \
	kernel/syscall kernel/mm kernel/fs kernel/sched kernel/sync kernel/net kernel/lib
C_SOURCES := $(foreach d,$(KERNEL_DIRS),$(wildcard $(d)/*.c))
# Every kernel header, as a coarse prerequisite for every object. Without
# this, editing a .h leaves stale .o files behind and a genuinely broken
# tree can appear to build clean.
C_HEADERS := $(foreach d,$(KERNEL_DIRS),$(wildcard $(d)/*.h))
C_OBJECTS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/isr_stubs.o $(BUILD_DIR)/context_switch.o $(BUILD_DIR)/syscall_entry.o $(BUILD_DIR)/fork_trampoline.o $(BUILD_DIR)/sigframe.o $(BUILD_DIR)/ap_trampoline.o

.PHONY: all build iso run test fresh-disks clean disk-image font-regen font-check render-check

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
USER_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -msse3 -mssse3 -msse4.1 -msse4.2 -mcmodel=large -fno-pic -ftls-model=local-exec -static -nostdlib -Wall -Wextra -std=gnu11 -O2 -I$(LIB_DIR)/include

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
$(MUSL_LIB):
	third_party/shim/apply.sh
	cd $(MUSL_DIR) && ./configure --target=x86_64 --disable-shared \
		CC=$(CC) AR=$(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-ar \
		RANLIB=$(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-ranlib \
		CFLAGS="-mcmodel=large -fno-pic -mno-red-zone -O2" >/dev/null
	$(MAKE) -C $(MUSL_DIR) -j$(shell nproc) >/dev/null

.PHONY: musl
musl: $(MUSL_LIB)

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

$(USERLAND_BUILD)/MPITEST.ELF: $(USERLAND_DIR)/mpitest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/mpitest.c -L$(LIB_BUILD) -lneoos

$(DISK_IMG): $(USERLAND_BUILD)/SPIN.ELF $(USERLAND_BUILD)/CHILD.ELF $(USERLAND_BUILD)/PARENT.ELF $(USERLAND_BUILD)/LOOPER.ELF $(USERLAND_BUILD)/YIELDER.ELF $(USERLAND_BUILD)/FAULTER.ELF $(USERLAND_BUILD)/FILEIO.ELF $(USERLAND_BUILD)/SSE_TEST.ELF $(USERLAND_BUILD)/FORKTEST.ELF $(USERLAND_BUILD)/EXECTARG.ELF $(USERLAND_BUILD)/MOUNTTST.ELF $(USERLAND_BUILD)/VFSTEST.ELF $(USERLAND_BUILD)/VTTEST.ELF $(USERLAND_BUILD)/ACTIVETTYTEST.ELF $(USERLAND_BUILD)/VTSWITCHTEST.ELF $(USERLAND_BUILD)/TERM.ELF $(USERLAND_BUILD)/TERMCHILD.ELF $(USERLAND_BUILD)/THRDTEST.ELF $(USERLAND_BUILD)/SIGTEST.ELF $(USERLAND_BUILD)/AVXTEST.ELF $(USERLAND_BUILD)/MMAPTEST.ELF $(USERLAND_BUILD)/REBTEST.ELF $(USERLAND_BUILD)/ORPHANTEST.ELF $(USERLAND_BUILD)/INIT.ELF $(USERLAND_BUILD)/FBTEST.ELF $(USERLAND_BUILD)/POLLTEST.ELF $(USERLAND_BUILD)/POLLTRUNC.ELF $(USERLAND_BUILD)/TLBSTORM.ELF $(USERLAND_BUILD)/FORKSTORM.ELF $(USERLAND_BUILD)/PTYCHURN.ELF $(USERLAND_BUILD)/SIGSTORM.ELF $(USERLAND_BUILD)/POLLSTORM.ELF $(USERLAND_BUILD)/FAULTFLOOD.ELF $(USERLAND_BUILD)/PTYTEST.ELF $(USERLAND_BUILD)/SMPTEST.ELF $(USERLAND_BUILD)/EVTEST.ELF $(USERLAND_BUILD)/IPCTEST.ELF $(USERLAND_BUILD)/PIPETEST.ELF $(USERLAND_BUILD)/TLSTEST.ELF $(USERLAND_BUILD)/NETTEST.ELF $(USERLAND_BUILD)/MPITEST.ELF $(USERLAND_BUILD)/CWDTEST.ELF $(USERLAND_BUILD)/STATTEST.ELF $(USERLAND_BUILD)/DIRTEST.ELF $(USERLAND_BUILD)/LFNTEST.ELF $(USERLAND_BUILD)/TIER0.ELF $(USERLAND_BUILD)/MUSLHELO.ELF $(USERLAND_BUILD)/TTYTEST.ELF
	mkdir -p $(DISK_SRC)/DIR
	printf 'Hello from NeoOS FAT16!\n' > $(DISK_SRC)/HELLO.TXT
	head -c 8192 /dev/zero | tr '\0' 'N' > $(DISK_SRC)/BIGFILE.TXT
	printf 'nested file contents\n' > $(DISK_SRC)/DIR/NESTED.TXT
	printf 'a long name survived the round trip\n' > "$(DISK_SRC)/A Long File Name.txt"
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=32 status=none
	mkfs.fat -F 16 $(DISK_IMG)
	mcopy -i $(DISK_IMG) $(DISK_SRC)/HELLO.TXT ::HELLO.TXT
	mcopy -i $(DISK_IMG) "$(DISK_SRC)/A Long File Name.txt" "::A Long File Name.txt"
	mcopy -i $(DISK_IMG) $(DISK_SRC)/BIGFILE.TXT ::BIGFILE.TXT
	mmd -i $(DISK_IMG) ::DIR
	mcopy -i $(DISK_IMG) $(DISK_SRC)/DIR/NESTED.TXT ::DIR/NESTED.TXT
	mmd -i $(DISK_IMG) ::BIN
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SPIN.ELF ::BIN/SPIN.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/CHILD.ELF ::BIN/CHILD.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/PARENT.ELF ::BIN/PARENT.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/LOOPER.ELF ::BIN/LOOPER.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/YIELDER.ELF ::BIN/YIELDER.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/FAULTER.ELF ::BIN/FAULTER.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/FILEIO.ELF ::BIN/FILEIO.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SSE_TEST.ELF ::BIN/SSE_TEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/FORKTEST.ELF ::BIN/FORKTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/EXECTARG.ELF ::BIN/EXECTARG.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/MOUNTTST.ELF ::BIN/MOUNTTST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/VFSTEST.ELF ::BIN/VFSTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/VTTEST.ELF ::BIN/VTTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/ACTIVETTYTEST.ELF ::BIN/ACTIVETTYTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/VTSWITCHTEST.ELF ::BIN/VTSWITCHTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/TERM.ELF ::BIN/TERM.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/TERMCHILD.ELF ::BIN/TERMCHILD.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/THRDTEST.ELF ::BIN/THRDTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SIGTEST.ELF ::BIN/SIGTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/AVXTEST.ELF ::BIN/AVXTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/MMAPTEST.ELF ::BIN/MMAPTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/REBTEST.ELF ::BIN/REBTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/ORPHANTEST.ELF ::BIN/ORPHANTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/FBTEST.ELF ::BIN/FBTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/POLLTEST.ELF ::BIN/POLLTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/POLLTRUNC.ELF ::BIN/POLLTRUNC.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/TLBSTORM.ELF ::BIN/TLBSTORM.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/FORKSTORM.ELF ::BIN/FORKSTORM.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/PTYCHURN.ELF ::BIN/PTYCHURN.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SIGSTORM.ELF ::BIN/SIGSTORM.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/POLLSTORM.ELF ::BIN/POLLSTORM.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/FAULTFLOOD.ELF ::BIN/FAULTFLOOD.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/PTYTEST.ELF ::BIN/PTYTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SMPTEST.ELF ::BIN/SMPTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/EVTEST.ELF ::BIN/EVTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/IPCTEST.ELF ::BIN/IPCTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/PIPETEST.ELF ::BIN/PIPETEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/TLSTEST.ELF ::BIN/TLSTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/NETTEST.ELF ::BIN/NETTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/MPITEST.ELF ::BIN/MPITEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/CWDTEST.ELF ::BIN/CWDTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/STATTEST.ELF ::BIN/STATTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/DIRTEST.ELF ::BIN/DIRTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/LFNTEST.ELF ::BIN/LFNTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/TIER0.ELF ::BIN/TIER0.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/MUSLHELO.ELF ::BIN/MUSLHELO.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/TTYTEST.ELF ::BIN/TTYTEST.ELF
	mmd -i $(DISK_IMG) ::SBIN
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/INIT.ELF ::SBIN/INIT.ELF
	mmd -i $(DISK_IMG) ::ETC
	printf '%s\n' \
	  '# NeoOS test workload -- launched by /SBIN/INIT (see userland/init.c)' \
	  '# TERM runs first as a wait entry: it claims the active tty +' \
	  '# framebuffer, renders TERMCHILD, self-checks, and exits before the' \
	  '# key-injecting suites (ttytest, evtest, polltest) start.' \
	  'wait /BIN/TERM.ELF' \
	  '# VTSWITCHTEST is a wait entry for the same reason: the active VT' \
	  '# is global state, and /dev/CONSOLE follows it. A concurrent' \
	  '# ttytest that set ICANON off through /dev/CONSOLE and read it' \
	  '# back after a switch got two DIFFERENT terminals, and reported' \
	  '# that the setting had not stuck.' \
	  'wait /BIN/VTSWITCHTEST.ELF' \
	  'spawn /BIN/PARENT.ELF' \
	  'spawn /BIN/LOOPER.ELF' \
	  'spawn /BIN/LOOPER.ELF' \
	  'spawn /BIN/YIELDER.ELF' \
	  'spawn /BIN/VFSTEST.ELF' \
	  'spawn /BIN/VTTEST.ELF' \
	  'spawn /BIN/ACTIVETTYTEST.ELF' \
	  'spawn /BIN/CWDTEST.ELF' \
	  'spawn /BIN/STATTEST.ELF' \
	  'spawn /BIN/DIRTEST.ELF' \
	  'spawn /BIN/LFNTEST.ELF' \
	  'spawn /BIN/TIER0.ELF' \
	  'spawn /BIN/MUSLHELO.ELF' \
	  'spawn /BIN/TTYTEST.ELF' \
	  'spawn /BIN/THRDTEST.ELF' \
	  'spawn /BIN/SIGTEST.ELF' \
	  'spawn /BIN/FAULTER.ELF' \
	  'spawn /BIN/AVXTEST.ELF' \
	  'spawn /BIN/MMAPTEST.ELF' \
	  'spawn /BIN/REBTEST.ELF' \
	  'spawn /BIN/ORPHANTEST.ELF' \
	  'spawn /BIN/FBTEST.ELF' \
	  'spawn /BIN/POLLTEST.ELF' \
	  'spawn /BIN/POLLTRUNC.ELF' \
	  'spawn /BIN/TLBSTORM.ELF' \
	  'spawn /BIN/FORKSTORM.ELF' \
	  '# PTYCHURN is a wait entry: it deliberately holds the ENTIRE pty' \
	  '# pool (all 16) for the length of a round, so anything opening a' \
	  '# pty beside it gets -ENFILE. ptytest did exactly that.' \
	  'wait /BIN/PTYCHURN.ELF' \
	  'spawn /BIN/SIGSTORM.ELF' \
	  '# POLLSTORM is a wait entry: it measures poll-broadcast traffic in' \
	  '# two windows and compares them, so the background load has to be' \
	  '# the same in both. Run beside the rest of the suite it swung 4.5x.' \
	  'wait /BIN/POLLSTORM.ELF' \
	  'spawn /BIN/PTYTEST.ELF' \
	  'spawn /BIN/SMPTEST.ELF' \
	  'spawn /BIN/EVTEST.ELF' \
	  'spawn /BIN/IPCTEST.ELF' \
	  'spawn /BIN/PIPETEST.ELF' \
	  'spawn /BIN/TLSTEST.ELF' \
	  'spawn /BIN/NETTEST.ELF' \
	  'spawn /BIN/MPITEST.ELF' \
	  > $(DISK_SRC)/INITTAB
	mcopy -i $(DISK_IMG) $(DISK_SRC)/INITTAB ::ETC/INITTAB

# 64MB, not 32: FAT32 needs at least 65525 clusters, and mkfs.fat warns
# that a 32MB image falls below that. Depends on $(DISK_IMG) only so
# that rule's mkdir of $(DISK_SRC) has already run.
$(DISK2_IMG): $(DISK_IMG)
	dd if=/dev/zero of=$(DISK2_IMG) bs=1M count=64 status=none
	mkfs.fat -F 32 $(DISK2_IMG)
	printf 'Hello from the FAT32 volume!\n' > $(DISK_SRC)/FAT32.TXT
	mcopy -i $(DISK2_IMG) $(DISK_SRC)/FAT32.TXT ::FAT32.TXT
	mmd -i $(DISK2_IMG) ::SUB
	printf 'nested on fat32\n' > $(DISK_SRC)/F32NEST.TXT
	mcopy -i $(DISK2_IMG) $(DISK_SRC)/F32NEST.TXT ::SUB/F32NEST.TXT

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

QEMU_COMMON := $(QEMU_MACHINE) -smp $(SMP_CPUS) -boot order=d \
	-cdrom $(BUILD_DIR)/neoos.iso \
	-drive file=$(DISK_IMG),format=raw -drive file=$(DISK2_IMG),format=raw \
	-vga std \
	-no-reboot

# -vga std: the plain Bochs-VBE standard VGA -- a dumb linear
# framebuffer, no acceleration, no guest driver. It is QEMU's current
# default on `pc`, but naming it keeps GRUB's Multiboot2 framebuffer
# request (and so /dev/fb0) working if that default ever changes.

run: iso disk-image
	qemu-system-x86_64 $(QEMU_COMMON)

# Headless boot with COM1 captured to a file. Fails if any selftest
# reported FAILED, or if the boot never reached the marker (a hang or a
# triple fault).
#
# The kernel powers off via ACPI (outw 0x2000 -> 0x604) as soon as the
# last user process exits (kernel/sched/proc.c:user_proc_exited), so a
# healthy run exits QEMU in ~15s. BOOT_TIMEOUT is now the HANG detector,
# not the normal exit path -- if it fires, something deadlocked. The
# qemu line keeps its leading `-` so a genuine timeout is still surfaced
# by the marker check rather than aborting the recipe.
BOOT_TIMEOUT ?= 60
BOOT_MARKER  ?= NeoOS: interrupts enabled, starting scheduler

# A suite that never RUNS is not a pass. Grepping only for FAILED lets a
# silently-missing suite look identical to a green one -- which it did,
# until a scheduler bug stopped three of the five from ever reporting and
# `make test` still said PASS. grep -F because "[vfstest]" is a character
# class to a regex, and would never match the literal brackets.
REQUIRED_MARKERS := \
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
	find $(BUILD_DIR) -name '*.o' -delete

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
test: clean-kernel fresh-disks iso disk-image
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

# faultflood needs the machine to ITSELF: it deliberately drives the
# system out of frames, and a `wait` entry does not stop tests already
# spawned, so in the shared suite it starved whatever was still running
# (forkstorm watched free frames go 29377 -> 319). It gets its own boot
# with an INITTAB containing nothing else.
faultflood: iso disk-image
	@printf '%s\n' \
	  '# generated by `make faultflood` -- this test runs alone' \
	  'wait /BIN/FAULTFLOOD.ELF' \
	  > $(BUILD_DIR)/disk-src/INITTAB.solo
	mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/disk-src/INITTAB.solo ::ETC/INITTAB
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
