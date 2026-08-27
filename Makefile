CC := $(HOME)/opt/cross-x86_64-elf/bin/x86_64-elf-gcc
AS := nasm

CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -Wall -Wextra -std=gnu11 -O2 -Ikernel
ASFLAGS := -f elf64

BUILD_DIR := build
ISO_DIR := iso

C_SOURCES := $(wildcard kernel/*.c) $(wildcard kernel/mm/*.c) $(wildcard kernel/fs/*.c) $(wildcard kernel/sched/*.c)
# Every kernel header, as a coarse prerequisite for every object. Without
# this, editing a .h leaves stale .o files behind and a genuinely broken
# tree can appear to build clean.
C_HEADERS := $(wildcard kernel/*.h) $(wildcard kernel/mm/*.h) $(wildcard kernel/fs/*.h) $(wildcard kernel/sched/*.h)
C_OBJECTS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/isr_stubs.o $(BUILD_DIR)/context_switch.o $(BUILD_DIR)/syscall_entry.o $(BUILD_DIR)/fork_trampoline.o $(BUILD_DIR)/sigframe.o

.PHONY: all build iso run clean disk-image

all: build

build: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR)/boot.o: boot/boot.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) boot/boot.asm -o $(BUILD_DIR)/boot.o

$(BUILD_DIR)/gdt_flush.o: kernel/gdt_flush.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/gdt_flush.asm -o $(BUILD_DIR)/gdt_flush.o

$(BUILD_DIR)/isr_stubs.o: kernel/isr.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/isr.asm -o $(BUILD_DIR)/isr_stubs.o

$(BUILD_DIR)/context_switch.o: kernel/context_switch.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/context_switch.asm -o $(BUILD_DIR)/context_switch.o

$(BUILD_DIR)/syscall_entry.o: kernel/syscall_entry.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/syscall_entry.asm -o $(BUILD_DIR)/syscall_entry.o

$(BUILD_DIR)/fork_trampoline.o: kernel/fork_trampoline.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/fork_trampoline.asm -o $(BUILD_DIR)/fork_trampoline.o

$(BUILD_DIR)/sigframe.o: kernel/sigframe.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/sigframe.asm -o $(BUILD_DIR)/sigframe.o

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
USER_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -msse3 -mssse3 -msse4.1 -msse4.2 -mcmodel=large -fno-pic -static -nostdlib -Wall -Wextra -std=gnu11 -O2 -I$(LIB_DIR)/include

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

$(USERLAND_BUILD)/VFSTEST.ELF: $(USERLAND_DIR)/vfstest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/vfstest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/THRDTEST.ELF: $(USERLAND_DIR)/threadtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/threadtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/SIGTEST.ELF: $(USERLAND_DIR)/sigtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/sigtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/AVXTEST.ELF: $(USERLAND_DIR)/avxtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -mavx -mavx2 -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/avxtest.c -L$(LIB_BUILD) -lneoos

$(USERLAND_BUILD)/MMAPTEST.ELF: $(USERLAND_DIR)/mmaptest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/mmaptest.c -L$(LIB_BUILD) -lneoos

$(DISK_IMG): $(USERLAND_BUILD)/SPIN.ELF $(USERLAND_BUILD)/CHILD.ELF $(USERLAND_BUILD)/PARENT.ELF $(USERLAND_BUILD)/LOOPER.ELF $(USERLAND_BUILD)/YIELDER.ELF $(USERLAND_BUILD)/FAULTER.ELF $(USERLAND_BUILD)/FILEIO.ELF $(USERLAND_BUILD)/SSE_TEST.ELF $(USERLAND_BUILD)/FORKTEST.ELF $(USERLAND_BUILD)/EXECTARG.ELF $(USERLAND_BUILD)/MOUNTTST.ELF $(USERLAND_BUILD)/VFSTEST.ELF $(USERLAND_BUILD)/THRDTEST.ELF $(USERLAND_BUILD)/SIGTEST.ELF $(USERLAND_BUILD)/AVXTEST.ELF $(USERLAND_BUILD)/MMAPTEST.ELF
	mkdir -p $(DISK_SRC)/DIR
	printf 'Hello from NeoOS FAT16!\n' > $(DISK_SRC)/HELLO.TXT
	head -c 8192 /dev/zero | tr '\0' 'N' > $(DISK_SRC)/BIGFILE.TXT
	printf 'nested file contents\n' > $(DISK_SRC)/DIR/NESTED.TXT
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=32 status=none
	mkfs.fat -F 16 $(DISK_IMG)
	mcopy -i $(DISK_IMG) $(DISK_SRC)/HELLO.TXT ::HELLO.TXT
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
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/THRDTEST.ELF ::BIN/THRDTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/SIGTEST.ELF ::BIN/SIGTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/AVXTEST.ELF ::BIN/AVXTEST.ELF
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/MMAPTEST.ELF ::BIN/MMAPTEST.ELF

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

run: iso disk-image
	qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom $(BUILD_DIR)/neoos.iso -drive file=$(DISK_IMG),format=raw -drive file=$(DISK2_IMG),format=raw

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR)
