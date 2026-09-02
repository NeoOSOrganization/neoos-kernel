#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("/mnt/fat32.txt", O_RDONLY);
    if (fd < 0) { printf("[mounttest] FAILED: open returned %d\n", fd); return 1; }

    int busy = umount("/mnt");
    printf("[mounttest] umount while open returned %d (want -16)\n", busy);

    close(fd);
    int ok = umount("/mnt");
    printf("[mounttest] umount after close returned %d (want 0)\n", ok);

    int gone = open("/mnt/fat32.txt", O_RDONLY);
    printf("[mounttest] open after umount returned %d (want negative)\n", gone);

    int re = mount("hd1", "/mnt", "fat");
    printf("[mounttest] remount returned %d (want 0)\n", re);
    return 0;
}
