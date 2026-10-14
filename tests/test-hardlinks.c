/* Where hard links work on the console, and why they fail where they don't. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *dirs[] = {
    "/user/data",
    "/data",
    "/data/PCSX2/games",
    "/data/PSXS5/games",
    "/data/homebrew",
    "/data/homebrew/PPSA99169/content/ps2",
    NULL,
};

/* From folder, to folder, what it tests. */
static const char *pairs[][3] = {
    {"/data/PCSX2/games", "/data/PCSX2/games", "A: same folder"},
    {"/data/PCSX2/games", "/data/PSXS5/games", "B: two folders under /data"},
    {"/data/homebrew/PPSA99169/content/ps2", "/data/homebrew/PPSA99169/content/ps2", "C: inside the homebrew mount"},
    {"/data/homebrew/PPSA99169/content/ps2", "/data/PCSX2/games", "D: across the mount (what RommPS does)"},
    {"/user/data/PCSX2/games", "/user/data/PCSX2/games", "E1: Same folder through underlying path"},
    {"/user/data/homebrew/PPSA99169/content/ps2", "/user/data/PCSX2/games", "E2: What we need to do through underlying path"},
    {NULL, NULL, NULL},
};

static void show_mount(const char *dir) {
    struct statfs fs;
    struct stat st;
    if (statfs(dir, &fs) != 0 || stat(dir, &st) != 0) {
        printf("%s: %s\n", dir, strerror(errno));
        return;
    }
    printf("%s\n  device %llu: %s mounted on %s (%s)\n", dir, (unsigned long long)st.st_dev, fs.f_mntfromname,
           fs.f_mntonname, fs.f_fstypename);
}

static void try_link(const char *from_dir, const char *to_dir, const char *what) {
    char from[512], to[512];
    snprintf(from, sizeof from, "%s/rommps-linktest-a", from_dir);
    snprintf(to, sizeof to, "%s/rommps-linktest-b", to_dir);
    printf("%s\n  %s -> %s\n", what, from_dir, to_dir);
    int fd = open(from, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("  can't create the test file: %s\n", strerror(errno));
        return;
    }
    if (write(fd, "test\n", 5) != 5) printf("  short write\n");
    close(fd);
    unlink(to);
    if (link(from, to) == 0) {
        struct stat a, b;
        stat(from, &a);
        stat(to, &b);
        printf("  linked: inodes %llu and %llu, %u links\n", (unsigned long long)a.st_ino,
               (unsigned long long)b.st_ino, (unsigned)a.st_nlink);
    } else {
        printf("  failed: %s (errno %d)\n", strerror(errno), errno);
    }
    unlink(to);
    unlink(from);
}

static void try_symlink(const char *dir) {
    char target[512], link_path[512];
    snprintf(target, sizeof target, "%s/rommps-linktest-a", dir);
    snprintf(link_path, sizeof link_path, "%s/rommps-linktest-sym", dir);
    printf("S: symlink in %s\n", dir);
    int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("  can't create the test file: %s\n", strerror(errno));
        return;
    }
    close(fd);
    unlink(link_path);
    if (symlink(target, link_path) == 0) {
        struct stat st;
        printf("  made; lstat says %s, stat %s\n", lstat(link_path, &st) == 0 && S_ISLNK(st.st_mode) ? "a link" : "not a link",
               stat(link_path, &st) == 0 ? "reaches the file" : "can't reach the file");
    } else {
        printf("  failed: %s (errno %d)\n", strerror(errno), errno);
    }
    unlink(link_path);
    unlink(target);
}

int main(void) {
    printf("== running as uid %d, euid %d\n", (int)getuid(), (int)geteuid());
    printf("== mounts\n");
    for (int i = 0; dirs[i]; i++) show_mount(dirs[i]);
    printf("== links\n");
    for (int i = 0; pairs[i][0]; i++) try_link(pairs[i][0], pairs[i][1], pairs[i][2]);
    try_symlink("/data/PCSX2/games");
    try_symlink("/user/data/PCSX2/games");
    try_symlink("/system_tmp");
    printf("== done\n");
    fflush(stdout);
    return 0;
}