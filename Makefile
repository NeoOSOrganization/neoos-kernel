CC := x86_64-elf-gcc
AS := nasm
LIBNEOOS_DIR ?= ../neoos-libneoos/build-output

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
	kernel/drivers/pci kernel/drivers/virtio kernel/drivers/net kernel/drivers/audio \
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

$(BUILD_DIR)/kernel.elf: $(ASM_OBJECTS) $(C_OBJECTS) $(BUILD_DIR)/embedfs_table.o linker.ld
	$(CC) -T linker.ld -o $(BUILD_DIR)/kernel.elf -ffreestanding -O2 -nostdlib \
		$(ASM_OBJECTS) $(C_OBJECTS) $(BUILD_DIR)/embedfs_table.o \
		$$(cat $(BUILD_DIR)/embedfs-objs.txt 2>/dev/null) -lgcc

iso: $(BUILD_DIR)/kernel.elf
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD_DIR)/kernel.elf $(ISO_DIR)/boot/kernel.elf
	cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD_DIR)/neoos.iso $(ISO_DIR)

DISK_IMG := $(BUILD_DIR)/disk.img
DISK2_IMG := $(BUILD_DIR)/disk2.img
DISK_SRC := $(BUILD_DIR)/disk-src


USERLAND_DIR := userland
USERLAND_BUILD := $(BUILD_DIR)/userland
USER_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone -msse3 -mssse3 -msse4.1 -msse4.2 -mcmodel=large -fno-pic -ftls-model=local-exec -static -nostdlib -Wall -Wextra -std=gnu11 -O2 -I$(LIBNEOOS_DIR)/include -Ishared

# ---- embedfs ----------------------------------------------------------
#
# Boot-critical apps (init/login/term/nsh) are ALWAYS embedded --
# without init.nex there is nothing for the kernel to spawn as PID 1
# (kernel/kernel.c). Everything else (the regression suite, ports) is
# optional: EMBED_DIRS is empty by default, so a bare `make` embeds
# only these four. See docs/superpowers/specs/
# 2026-09-05-embedded-test-and-app-architecture.md.
EMBED_DIRS ?=

$(DISK_SRC)/nex-embed-boot/init.nex: $(USERLAND_BUILD)/INIT.ELF userland/boot-apps/init.test.json
	@mkdir -p $(DISK_SRC)/nex-embed-boot
	./tools/nexify.sh $(USERLAND_BUILD)/INIT.ELF $@
	cp userland/boot-apps/init.test.json $(DISK_SRC)/nex-embed-boot/

$(DISK_SRC)/nex-embed-boot/login.nex: $(USERLAND_BUILD)/LOGIN.ELF userland/boot-apps/login.test.json
	@mkdir -p $(DISK_SRC)/nex-embed-boot
	./tools/nexify.sh $(USERLAND_BUILD)/LOGIN.ELF $@
	cp userland/boot-apps/login.test.json $(DISK_SRC)/nex-embed-boot/

$(DISK_SRC)/nex-embed-boot/term.nex: $(USERLAND_BUILD)/TERM.ELF userland/boot-apps/term.test.json
	@mkdir -p $(DISK_SRC)/nex-embed-boot
	./tools/nexify.sh $(USERLAND_BUILD)/TERM.ELF $@
	cp userland/boot-apps/term.test.json $(DISK_SRC)/nex-embed-boot/

$(DISK_SRC)/nex-embed-boot/nsh.nex: $(USERLAND_BUILD)/NSH.ELF userland/boot-apps/nsh.test.json
	@mkdir -p $(DISK_SRC)/nex-embed-boot
	./tools/nexify.sh $(USERLAND_BUILD)/NSH.ELF $@
	cp userland/boot-apps/nsh.test.json $(DISK_SRC)/nex-embed-boot/

$(DISK_SRC)/nex-embed-boot/looper.nex: $(USERLAND_BUILD)/LOOPER.ELF userland/boot-apps/looper.test.json
	@mkdir -p $(DISK_SRC)/nex-embed-boot
	./tools/nexify.sh $(USERLAND_BUILD)/LOOPER.ELF $@
	cp userland/boot-apps/looper.test.json $(DISK_SRC)/nex-embed-boot/

$(DISK_SRC)/nex-embed-boot/termchild.nex: $(USERLAND_BUILD)/TERMCHILD.ELF userland/boot-apps/termchild.test.json
	@mkdir -p $(DISK_SRC)/nex-embed-boot
	./tools/nexify.sh $(USERLAND_BUILD)/TERMCHILD.ELF $@
	cp userland/boot-apps/termchild.test.json $(DISK_SRC)/nex-embed-boot/

$(BUILD_DIR)/embedfs_table.c: $(DISK_SRC)/nex-embed-boot/init.nex $(DISK_SRC)/nex-embed-boot/login.nex \
                               $(DISK_SRC)/nex-embed-boot/term.nex $(DISK_SRC)/nex-embed-boot/nsh.nex \
                               $(DISK_SRC)/nex-embed-boot/looper.nex $(DISK_SRC)/nex-embed-boot/termchild.nex
	LD=x86_64-elf-ld python3 tools/gen-embedfs.py \
		$(BUILD_DIR)/embedfs_table.c $(DISK_SRC)/nex-embed-boot $(EMBED_DIRS)

$(BUILD_DIR)/embedfs_table.o: $(BUILD_DIR)/embedfs_table.c
	$(CC) $(CFLAGS) -c $(BUILD_DIR)/embedfs_table.c -o $(BUILD_DIR)/embedfs_table.o


# musl source lives in the separate neoos-musl repo -- build it there
# and point MUSL_DIR at its build-output (include/, lib/libc.a,
# lib/crt1.o). `?=` so it can still be overridden for local iteration.
MUSL_DIR   ?= ../neoos-musl/build-output
MUSL_LIB   := $(MUSL_DIR)/lib/libc.a
MUSL_CFLAGS := -static -nostdlib -nostdinc -ffreestanding \
	-mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
	-isystem $(MUSL_DIR)/include

$(MUSL_LIB):
	@echo "error: $(MUSL_LIB) not found." >&2
	@echo "Build it in neoos-musl first: cd ../neoos-musl && make KERNEL_SHIM_DIR=$(CURDIR)/third_party/shim" >&2
	@exit 1

# login links against musl, not libneoos, for one reason: crypt().
# musl ships SHA-256-crypt, SHA-512-crypt, MD5 and bcrypt back-ends, all
# pure computation with no files and no randomness -- so NeoOS gets real
# password hashing with nothing ported and nothing hand-rolled.
$(USERLAND_BUILD)/LOGIN.ELF: $(USERLAND_DIR)/musl/login.c $(USERLAND_DIR)/user.ld $(MUSL_LIB)
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(MUSL_CFLAGS) -T $(USERLAND_DIR)/user.ld -z noexecstack -o $@ \
		$(MUSL_DIR)/lib/crt1.o $(USERLAND_DIR)/musl/login.c \
		-L$(MUSL_DIR)/lib -lc -lgcc





TERM_SRC := $(USERLAND_DIR)/term/main.c $(USERLAND_DIR)/term/vt.c $(USERLAND_DIR)/term/render.c $(USERLAND_DIR)/term/palette.c $(USERLAND_DIR)/term/font_term.c
TERM_HDR := $(USERLAND_DIR)/term/vt.h $(USERLAND_DIR)/term/render.h $(USERLAND_DIR)/term/palette.h $(USERLAND_DIR)/term/font_term.h

$(USERLAND_BUILD)/TERM.ELF: $(TERM_SRC) $(TERM_HDR) $(USERLAND_DIR)/user.ld $(LIBNEOOS_DIR)/lib/crt0.o $(LIBNEOOS_DIR)/lib/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -I$(USERLAND_DIR)/term -T $(USERLAND_DIR)/user.ld -o $@ \
	    $(LIBNEOOS_DIR)/lib/crt0.o $(TERM_SRC) -L$(LIBNEOOS_DIR)/lib -lneoos

$(USERLAND_BUILD)/INIT.ELF: $(USERLAND_DIR)/init.c $(USERLAND_DIR)/user.ld $(LIBNEOOS_DIR)/lib/crt0.o $(LIBNEOOS_DIR)/lib/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIBNEOOS_DIR)/lib/crt0.o $(USERLAND_DIR)/init.c -L$(LIBNEOOS_DIR)/lib -lneoos

$(USERLAND_BUILD)/NSH.ELF: $(USERLAND_DIR)/nsh.c $(USERLAND_DIR)/user.ld $(LIBNEOOS_DIR)/lib/crt0.o $(LIBNEOOS_DIR)/lib/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIBNEOOS_DIR)/lib/crt0.o $(USERLAND_DIR)/nsh.c -L$(LIBNEOOS_DIR)/lib -lneoos

# LOOPER and TERMCHILD are not optional test-suite content here despite
# living in userland/ like a test: kernel/smp/smp_selftest.c spawns
# /usr/tests/looper.nex directly (hardcoded path, not via inittab), and
# term/main.c execs /usr/tests/termchild.nex as its own self-check.
# Both are load-bearing for CORE_REQUIRED_MARKERS ("[smp] steal selftest
# passed", "[term] render ALL PASSED"), so -- like init/login/term/nsh
# -- they must be embedded unconditionally, not only via EMBED_DIRS.
$(USERLAND_BUILD)/LOOPER.ELF: $(USERLAND_DIR)/looper.c $(USERLAND_DIR)/user.ld $(LIBNEOOS_DIR)/lib/crt0.o $(LIBNEOOS_DIR)/lib/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIBNEOOS_DIR)/lib/crt0.o $(USERLAND_DIR)/looper.c -L$(LIBNEOOS_DIR)/lib -lneoos

$(USERLAND_BUILD)/TERMCHILD.ELF: $(USERLAND_DIR)/termchild.c $(USERLAND_DIR)/user.ld $(LIBNEOOS_DIR)/lib/crt0.o $(LIBNEOOS_DIR)/lib/libneoos.a
	@mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIBNEOOS_DIR)/lib/crt0.o $(USERLAND_DIR)/termchild.c -L$(LIBNEOOS_DIR)/lib -lneoos

$(DISK_IMG): $(BUILD_DIR)/embedfs_table.c $(USERLAND_BUILD)/TERM.ELF $(USERLAND_BUILD)/INIT.ELF $(USERLAND_BUILD)/NSH.ELF $(USERLAND_BUILD)/LOGIN.ELF
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
	@# Ports and the regression suite are not built by this repo at all
	@# (spec: "does NOT contain ports"/tests) -- they arrive purely via
	@# EMBED_DIRS, pointed at neoos-kernel-tests-common's and each
	@# port's build/ output. See the embedfs_table.c rule above.
	@echo "disk: EMBED_DIRS=$(EMBED_DIRS)"
	printf '%s\n' \
	  '# NeoOS test workload -- launched by /sbin/init.nex (see userland/init.c)' \
	  '# TERM runs first as a wait entry: it claims the active tty +' \
	  '# framebuffer, renders TERMCHILD, self-checks, and exits before the' \
	  '# key-injecting suites (ttytest, evtest, polltest) start.' \
	  'wait /bin/term.nex' \
	  > $(DISK_SRC)/inittab.base
	@# The rest of the suite (every /usr/tests/*.nex entry, plus BusyBox's
	@# bbspike/nshtest/bbsh) is data-driven now: each test/port ships a
	@# manifest (userland/tests.manifest.json,
	@# third_party/busybox-config/busybox.test.json) declaring its own
	@# inittab line(s) and WHERE to insert them (an "after" anchor to a
	@# previously-placed entry) -- see tools/apply-inittab-patch.py and
	@# docs/superpowers/specs/2026-09-05-embedded-test-and-app-architecture.md.
	@# This is exactly how the hand-tuned interleaving here (BBSPIKE
	@# right after thrdmany, PTYCHURN holding the whole pty pool alone,
	@# POLLSTORM needing the same background load in both windows) is
	@# preserved without hardcoding it.
	python3 tools/apply-inittab-patch.py $(DISK_SRC)/inittab.base \
		$(BUILD_DIR)/embedfs-inittab-patch.json > $(DISK_SRC)/inittab
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
	-audiodev wav,id=ac97wav,path=$(BUILD_DIR)/ac97-test.wav \
	-device AC97,audiodev=ac97wav,addr=0x6 \
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
	@$(MAKE) QUIET=1 clean-kernel iso disk-image
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
# Split from the old single list: these are KERNEL-INTERNAL selftests
# with no userland test binary behind them (they print straight from
# boot code), so they're not something any manifest could declare --
# they stay hardcoded. Everything that WAS a /usr/tests/*.nex or
# BusyBox marker is now collected from embedfs-markers.txt, generated
# by tools/gen-embedfs.py from userland/tests.manifest.json and
# third_party/busybox-config/busybox.test.json. This is what makes a
# bare `make test` (no test suite, no BusyBox) require a SMALLER set --
# see docs/superpowers/specs/2026-09-05-embedded-test-and-app-architecture.md.
CORE_REQUIRED_MARKERS := \
	"[pci] ALL PASSED" \
	"[virtio-net] ALL PASSED" \
	"[route] ALL PASSED" \
	"[arp] ALL PASSED" \
	"[icmp] ALL PASSED" \
	"[dhcp] ALL PASSED" \
	"[dns] ALL PASSED" \
	"[tcp] ALL PASSED" \
	"[smp] local timer selftest passed" \
	"[term] render ALL PASSED" \
	"[init] all entries exited -- powering off" \
	"[devfs] selftest passed" \
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
	"[input] selftest passed" \
	"[ac97] selftest passed" \
	"[ac97] /dev/snd selftest passed" \
	"[uaccess] selftest passed"

# `=` (recursive), not `:=`: this must re-read embedfs-markers.txt at
# RECIPE-EXECUTION time, after $(BUILD_DIR)/embedfs_table.c's rule has
# generated it -- a `:=` would freeze in whatever the file held (or its
# absence) at Makefile-PARSE time, before that rule has ever run.
REQUIRED_MARKERS = $(CORE_REQUIRED_MARKERS) $(shell sed 's/.*/"&"/' $(BUILD_DIR)/embedfs-markers.txt 2>/dev/null)

# The fat16 WRITE selftest creates /NEWDIR and /RT.TXT on the real disk
# image, and the image persists between runs -- so on the second boot
# mkdir(/NEWDIR) fails with "already exists" and every later `make test`
# reports a failure that has nothing to do with the change under test.
# Rebuilding the images makes each test run start from a known state.

# embedfs-obj/ excluded: it's a cache gen-embedfs.py manages itself,
# not a real Make prerequisite of anything (the link step reaches its
# contents via a shell command substitution, invisible to Make's
# dependency graph) -- sweeping it here with nothing forcing a
# regeneration (LIBNEOOS_DIR/MUSL_DIR are external and untouched by
# this rule) is a straight link failure, not a "gets rebuilt anyway".
clean-kernel:
	mkdir -p $(BUILD_DIR) && find $(BUILD_DIR) -name '*.o' -not -path '*/embedfs-obj/*' -delete

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
	@if [ ! -s $(BUILD_DIR)/ac97-test.wav ]; then \
		echo "AC97 WAV FILE MISSING OR EMPTY: no audio was captured"; exit 1; fi
	@wav_bytes=$$(stat -c%s $(BUILD_DIR)/ac97-test.wav 2>/dev/null || stat -f%z $(BUILD_DIR)/ac97-test.wav); \
	if [ "$$wav_bytes" -lt 1000 ]; then \
		echo "AC97 WAV FILE TOO SMALL ($$wav_bytes bytes): DMA likely did not run"; exit 1; fi
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
