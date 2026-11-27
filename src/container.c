#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cgroup.h"
#include "container.h"

#define STACK_SIZE (1024 * 1024)

struct child_args {
    const struct container_config *cfg;
    int sync[2];
};

static volatile sig_atomic_t container_pid;
static volatile sig_atomic_t received_signal;

static int setup_dev(void)
{
    if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC,
              "mode=755") == -1) {
        perror("bocal: mount /dev");
        return -1;
    }

    static const struct {
        const char *path;
        unsigned int major, minor;
    } devices[] = {
        { "/dev/null",    1, 3 },
        { "/dev/zero",    1, 5 },
        { "/dev/full",    1, 7 },
        { "/dev/random",  1, 8 },
        { "/dev/urandom", 1, 9 },
        { "/dev/tty",     5, 0 },
    };
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        if (mknod(devices[i].path, S_IFCHR | 0666,
                  makedev(devices[i].major, devices[i].minor)) == -1 ||
            chmod(devices[i].path, 0666) == -1) {
            fprintf(stderr, "bocal: %s : %s\n", devices[i].path,
                    strerror(errno));
            return -1;
        }
    }

    static const char *const links[][2] = {
        { "/proc/self/fd",   "/dev/fd" },
        { "/proc/self/fd/0", "/dev/stdin" },
        { "/proc/self/fd/1", "/dev/stdout" },
        { "/proc/self/fd/2", "/dev/stderr" },
    };
    for (size_t i = 0; i < sizeof links / sizeof links[0]; i++) {
        if (symlink(links[i][0], links[i][1]) == -1) {
            fprintf(stderr, "bocal: %s : %s\n", links[i][1],
                    strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int setup_loopback(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        perror("bocal: socket");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "lo");

    int ret = -1;
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == -1) {
        perror("bocal: ioctl SIOCGIFFLAGS");
    } else {
        ifr.ifr_flags |= IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) == -1)
            perror("bocal: ioctl SIOCSIFFLAGS");
        else
            ret = 0;
    }
    close(fd);
    return ret;
}

static int enter_rootfs(const char *rootfs)
{
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) == -1) {
        perror("bocal: mount (image)");
        return -1;
    }

    if (chdir(rootfs) == -1) {
        perror("bocal: chdir (image)");
        return -1;
    }

    if (syscall(SYS_pivot_root, ".", ".") == -1) {
        perror("bocal: pivot_root");
        return -1;
    }

    if (umount2(".", MNT_DETACH) == -1) {
        perror("bocal: umount2 (ancienne racine)");
        return -1;
    }

    if (chdir("/") == -1) {
        perror("bocal: chdir");
        return -1;
    }

    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == -1) {
        perror("bocal: mount /proc");
        return -1;
    }

    return setup_dev();
}

static int child_main(void *arg)
{
    struct child_args *args = arg;
    const struct container_config *cfg = args->cfg;

    if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
        perror("bocal: prctl");
        return 1;
    }

    close(args->sync[1]);
    char go;
    if (read(args->sync[0], &go, 1) != 1)
        return 1;
    close(args->sync[0]);

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
        perror("bocal: mount");
        return 1;
    }

    if (sethostname(cfg->hostname, strlen(cfg->hostname)) == -1) {
        perror("bocal: sethostname");
        return 1;
    }

    if (setup_loopback() == -1)
        return 1;

    if (enter_rootfs(cfg->rootfs) == -1)
        return 1;

    execvp(cfg->argv[0], cfg->argv);

    int err = errno;
    fprintf(stderr, "bocal: impossible de lancer « %s » : %s\n",
            cfg->argv[0], strerror(err));
    return err == ENOENT ? 127 : 126;
}

static void on_signal(int sig)
{
    received_signal = sig;
    if (container_pid > 0)
        kill(container_pid, SIGKILL);
}

static pid_t start_child(struct child_args *args)
{
    char *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        perror("bocal: mmap");
        return -1;
    }

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET |
                SIGCHLD;
    pid_t pid = clone(child_main, stack + STACK_SIZE, flags, args);
    int err = errno;

    munmap(stack, STACK_SIZE);

    if (pid == -1) {
        fprintf(stderr, "bocal: clone: %s\n", strerror(err));
        if (err == EPERM)
            fprintf(stderr, "bocal: il faut être root pour créer des "
                            "namespaces, relance la commande avec sudo\n");
    }
    return pid;
}

static int run_child(const struct container_config *cfg, const char *cgroup)
{
    struct child_args args = { .cfg = cfg };

    if (pipe2(args.sync, O_CLOEXEC) == -1) {
        perror("bocal: pipe2");
        return -1;
    }

    pid_t pid = start_child(&args);
    if (pid == -1) {
        close(args.sync[0]);
        close(args.sync[1]);
        return -1;
    }

    bool ok = (cgroup == NULL || cgroup_add(cgroup, pid) == 0) &&
              write(args.sync[1], "x", 1) == 1;

    close(args.sync[0]);
    close(args.sync[1]);

    container_pid = pid;
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    int status;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w == -1 && errno == EINTR);

    container_pid = 0;

    if (w == -1) {
        perror("bocal: waitpid");
        return -1;
    }
    if (!ok)
        return -1;
    if (received_signal)
        return 128 + received_signal;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

int container_run(const struct container_config *cfg)
{
    char cgroup[PATH_MAX];
    bool limited = cfg->memory > 0 || cfg->cpus > 0;
    if (limited &&
        cgroup_create(cgroup, sizeof cgroup, cfg->memory, cfg->cpus) == -1)
        return -1;

    int ret = run_child(cfg, limited ? cgroup : NULL);

    if (limited)
        cgroup_remove(cgroup);
    return ret;
}
