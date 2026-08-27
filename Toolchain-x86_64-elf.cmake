# CMake Toolchain File for x86_64-elf cross-compilation

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Specify the cross-compiler
set(CROSS_COMPILER_SYSROOT "${HOME}/opt/cross-x86_64-elf")
set(CMAKE_C_COMPILER "${CROSS_COMPILER_SYSROOT}/bin/x86_64-elf-gcc")
set(CMAKE_CXX_COMPILER "${CROSS_COMPILER_SYSROOT}/bin/x86_64-elf-g++")
set(CMAKE_ASM_COMPILER nasm)

# Add cross-compiler include directories for CLion indexing
include_directories(${CROSS_COMPILER_SYSROOT}/x86_64-elf/include)

# Configure ASM language
set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS asm)
set(CMAKE_ASM_COMPILE_OBJECT "<CMAKE_ASM_COMPILER> -f elf64 <INCLUDES> <DEFINES> <FLAGS> -o <OBJECT> <SOURCE>")

# Don't look for programs in the target system prefix (use native tools)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Skip compiler checks (they won't work for bare-metal cross-compilation)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_C_ABI_COMPILED 1)
set(CMAKE_CXX_ABI_COMPILED 1)

# Flags for freestanding environment
set(COMMON_FLAGS "-ffreestanding -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel")

set(CMAKE_C_FLAGS_INIT "${COMMON_FLAGS} -std=gnu11 -Wall -Wextra -O2")
set(CMAKE_ASM_FLAGS_INIT "-f elf64")
