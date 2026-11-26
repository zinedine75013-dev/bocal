/*
 * cgroup.c : limites de ressources avec les cgroups v2.
 *
 * Un cgroup est un dossier de /sys/fs/cgroup. On règle ses limites en
 * écrivant dans ses fichiers (memory.max, cpu.max...), puis on y place
 * des processus : le noyau applique ces limites à eux et à tous leurs
 * descendants.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cgroup.h"

/* Point de montage des cgroups v2. */
#define CGROUP_ROOT "/sys/fs/cgroup"

/* Période de la limite CPU, en microsecondes (100 ms). */
#define CPU_PERIOD 100000

/*
 * Écrit value dans le fichier dir/name. Renvoie 0, ou -1 (avec errno).
 * C'est ainsi qu'on parle au noyau : il vérifie la valeur reçue, et
 * write() échoue si elle ne lui convient pas.
 */
static int write_file(const char *dir, const char *name, const char *value)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd == -1)
        return -1;

    ssize_t n = write(fd, value, strlen(value));
    int err = errno;   /* close() pourrait modifier errno */
    close(fd);
    errno = err;
    return n == -1 ? -1 : 0;
}

/*
 * Active un contrôleur ("memory" ou "cpu") pour les cgroups créés sous la
 * racine : sans ça, les fichiers memory.max ou cpu.max n'existeraient pas
 * dans le nôtre. Sur la plupart des systèmes, systemd l'a déjà fait, et
 * le refaire ne change rien.
 */
static int enable_controller(const char *name)
{
    char value[16];
    snprintf(value, sizeof value, "+%s", name);
    if (write_file(CGROUP_ROOT, "cgroup.subtree_control", value) == -1) {
        fprintf(stderr, "bocal: impossible d'activer le contrôleur %s "
                        "des cgroups v2 : %s\n", name, strerror(errno));
        fprintf(stderr, "bocal: vérifie que « stat -fc %%T /sys/fs/cgroup » "
                        "affiche cgroup2fs\n");
        return -1;
    }
    return 0;
}

/* Écrit les limites dans les fichiers du cgroup. Renvoie 0, ou -1. */
static int write_limits(const char *cgroup, unsigned long long memory,
                        double cpus)
{
    char value[64];

    if (memory > 0) {
        /* memory.max : au-delà, le noyau tue un processus du cgroup. */
        snprintf(value, sizeof value, "%llu", memory);
        if (write_file(cgroup, "memory.max", value) == -1) {
            perror("bocal: memory.max");
            return -1;
        }

        /* Sans ça, une fois la limite atteinte, le conteneur pourrait
           continuer dans le swap au lieu d'être arrêté. Ce fichier
           n'existe pas si le noyau ne compte pas le swap par cgroup :
           on s'en passe alors. */
        if (write_file(cgroup, "memory.swap.max", "0") == -1 &&
            errno != ENOENT) {
            perror("bocal: memory.swap.max");
            return -1;
        }
    }

    if (cpus > 0) {
        /* cpu.max contient « quota période », en microsecondes : le
           conteneur peut calculer pendant quota µs toutes les période µs.
           Par exemple, 0.5 cœur donne « 50000 100000 ». Le + 0.5 arrondit
           au plus proche, au lieu de tronquer. */
        snprintf(value, sizeof value, "%ld %d",
                 (long)(cpus * CPU_PERIOD + 0.5), CPU_PERIOD);
        if (write_file(cgroup, "cpu.max", value) == -1) {
            perror("bocal: cpu.max");
            return -1;
        }
    }

    return 0;
}

int cgroup_create(char *path, size_t size,
                  unsigned long long memory, double cpus)
{
    if (memory > 0 && enable_controller("memory") == -1)
        return -1;
    if (cpus > 0 && enable_controller("cpu") == -1)
        return -1;

    /* Un cgroup est un simple dossier. On en crée un par conteneur, nommé
       d'après le PID de bocal pour qu'il soit unique. */
    snprintf(path, size, CGROUP_ROOT "/bocal-%d", (int)getpid());
    if (mkdir(path, 0755) == -1) {
        perror("bocal: création du cgroup");
        return -1;
    }

    if (write_limits(path, memory, cpus) == -1) {
        cgroup_remove(path);
        return -1;
    }
    return 0;
}

int cgroup_add(const char *path, pid_t pid)
{
    /* Écrire un PID dans cgroup.procs y déplace ce processus. */
    char value[32];
    snprintf(value, sizeof value, "%d", (int)pid);
    if (write_file(path, "cgroup.procs", value) == -1) {
        perror("bocal: cgroup.procs");
        return -1;
    }
    return 0;
}

void cgroup_remove(const char *path)
{
    /* Un cgroup se supprime avec rmdir(), comme un dossier vide : ses
       fichiers sont virtuels et disparaissent avec lui. */
    if (rmdir(path) == -1)
        perror("bocal: suppression du cgroup");
}
