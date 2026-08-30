cmake_minimum_required(VERSION 3.16)

# Set the toolchain file before project() call
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Toolchain-x86_64-elf.cmake" CACHE FILEPATH "CMake toolchain file" FORCE)
endif()

project(NeoOS C ASM)

# CMake configuration for bare-metal build
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_SKIP_RPATH TRUE)

# Kernel target (file indexing only; actual build via Makefile)
add_library(kernel_sources OBJECT
    kernel/acpi.c
    kernel/ata.c
    kernel/cpu.c
    kernel/elf.c
    kernel/gdt.c
    kernel/idt.c
    kernel/isr.c
    kernel/ioapic.c
    kernel/kernel.c
    kernel/keyboard.c
    kernel/lapic.c
    kernel/pic.c
    kernel/pit.c
    kernel/serial.c
    kernel/signal.c
    kernel/syscall.c
    kernel/timer.c
    kernel/tss.c
    kernel/vga.c
    kernel/waitq.c
    kernel/cpu_local.c
    kernel/lock.c
    kernel/mm/heap.c
    kernel/mm/paging.c
    kernel/mm/pmm.c
    kernel/mm/vma.c
    kernel/fs/vfs.c
    kernel/fs/blkcache.c
    kernel/fs/vnode_slab.c
    kernel/fs/fatfs.c
    kernel/fs/ramfs.c
    kernel/fs/devfs.c
    kernel/sched/proc.c
    kernel/sched/sched.c
    kernel/sched/thread.c
    kernel/sched/proc_table.c
    kernel/sched/thread_table.c
    kernel/sched/fd_table.c
    kernel/sched/pid_alloc.c
    kernel/sync/rcu.c
)

target_include_directories(kernel_sources PRIVATE kernel)
set_target_properties(kernel_sources PROPERTIES
    LANGUAGE C
    LINKER_LANGUAGE C
)

# Standard library target (file indexing only)
add_library(lib_sources OBJECT
    lib/dirent.c
    lib/stdio.c
    lib/syscall.c
    lib/string.c
)
target_include_directories(lib_sources PRIVATE lib/include)

# Userland programs (for indexing only; actual build via Makefile)
add_library(userland_spin OBJECT userland/spin.c)
add_library(userland_child OBJECT userland/child.c)
add_library(userland_parent OBJECT userland/parent.c)
add_library(userland_looper OBJECT userland/looper.c)
add_library(userland_fork_test OBJECT userland/fork_test.c)
add_library(userland_yielder OBJECT userland/yielder.c)
add_library(userland_sse_test OBJECT userland/sse_test.c)
add_library(userland_faulter OBJECT userland/faulter.c)
add_library(userland_fileio OBJECT userland/fileio.c)
add_library(userland_mounttest OBJECT userland/mounttest.c)
add_library(userland_vfstest OBJECT userland/vfstest.c)
add_library(userland_exec_target OBJECT userland/exec_target.c)

foreach(target userland_spin userland_child userland_parent userland_looper
               userland_fork_test userland_yielder userland_sse_test
               userland_faulter userland_fileio userland_mounttest
               userland_vfstest userland_exec_target)
    target_include_directories(${target} PRIVATE lib/include)
endforeach()

# Custom target that delegates to Makefile for actual building
# This allows you to build with CLion while using the Make-based build
add_custom_target(build_kernel ALL
    COMMAND make -C "${CMAKE_SOURCE_DIR}" build
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(build_iso
    COMMAND make -C "${CMAKE_SOURCE_DIR}" iso
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
