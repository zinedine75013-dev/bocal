/*
 * container.c : création du conteneur.
 *
 * Un conteneur n'est qu'un processus Linux ordinaire, lancé dans de
 * nouveaux "namespaces" : des vues privées sur certaines ressources du
 * système. Bocal en utilise quatre :
 *
 *   CLONE_NEWPID  numéros de processus : le conteneur démarre au PID 1
 *   CLONE_NEWUTS  nom d'hôte : le conteneur peut changer le sien
 *   CLONE_NEWNS   points de montage : ce qu'il monte reste chez lui
 *   CLONE_NEWNET  réseau : le conteneur n'a que sa propre interface lo
 *
 * Grâce à son namespace de montage, le conteneur peut ensuite changer de
 * racine : le dossier d'une image (Alpine) devient son "/", et il ne voit
 * plus aucun fichier de l'hôte. Il reçoit aussi un /proc et un /dev à lui.
 *
 * Enfin, si on lui demande des limites (--memory, --cpus), le conteneur
 * est placé dans un cgroup avant même que sa commande démarre.
 */
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

/* Taille de la pile du processus enfant : clone() ne la crée pas pour nous. */
#define STACK_SIZE (1024 * 1024)

/* Ce que le parent transmet à l'enfant. */
struct child_args {
    const struct container_config *cfg;
    int sync[2];   /* tube de synchronisation : [0] lecture, [1] écriture */
};

/* Variables partagées avec le gestionnaire de signal on_signal(). Le type
   volatile sig_atomic_t garantit qu'il les lit et les écrit correctement,
   même s'il interrompt le programme à n'importe quel moment. */
static volatile sig_atomic_t container_pid;
static volatile sig_atomic_t received_signal;

/*
 * Crée un /dev minimal : un tmpfs (un système de fichiers en mémoire) avec
 * les quelques fichiers spéciaux dont les programmes ont besoin. Le /dev
 * de l'hôte, lui, donnerait accès à tous ses périphériques, disques compris.
 * Renvoie 0, ou -1 en cas d'erreur.
 */
static int setup_dev(void)
{
    if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC,
              "mode=755") == -1) {
        perror("bocal: mount /dev");
        return -1;
    }

    /* Un fichier spécial désigne un pilote du noyau par deux numéros : le
       majeur (le pilote) et le mineur (le périphérique). Par exemple, 1,3
       est /dev/null, et 5,0 est /dev/tty, le terminal du processus. */
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
        /* mknod() crée le fichier spécial, mais applique le umask : le
           chmod() qui suit donne les droits voulus (lecture et écriture
           pour tous). */
        if (mknod(devices[i].path, S_IFCHR | 0666,
                  makedev(devices[i].major, devices[i].minor)) == -1 ||
            chmod(devices[i].path, 0666) == -1) {
            fprintf(stderr, "bocal: %s : %s\n", devices[i].path,
                    strerror(errno));
            return -1;
        }
    }

    /* Raccourcis habituels vers les fichiers ouverts du processus. */
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

/*
 * Allume l'interface réseau lo (127.0.0.1) du conteneur. Son namespace
 * réseau est vide : lo est sa seule interface, et elle démarre éteinte.
 * Le conteneur n'a donc accès ni au réseau de la machine, ni à Internet.
 * Renvoie 0, ou -1 en cas d'erreur.
 */
static int setup_loopback(void)
{
    /* Les réglages d'une interface passent par ioctl() sur un socket
       quelconque : SIOCGIFFLAGS lit ses drapeaux, SIOCSIFFLAGS les écrit. */
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
        ifr.ifr_flags |= IFF_UP;   /* le drapeau « allumée » */
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) == -1)
            perror("bocal: ioctl SIOCSIFFLAGS");
        else
            ret = 0;
    }
    close(fd);
    return ret;
}

/*
 * Fait du dossier de l'image la racine "/" du conteneur, puis lui monte un
 * /proc neuf et un /dev minimal. Appelée par l'enfant, déjà dans ses
 * nouveaux namespaces. Renvoie 0, ou -1 en cas d'erreur.
 */
static int enter_rootfs(const char *rootfs)
{
    /* pivot_root() exige que la nouvelle racine soit un point de montage,
       ce que le dossier de l'image n'est pas. On le monte donc sur
       lui-même (bind mount). Ce montage n'existe que dans le conteneur. */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) == -1) {
        perror("bocal: mount (image)");
        return -1;
    }

    /* On se place dans l'image. C'est fait après le montage, pour que le
       dossier courant soit le point de montage lui-même, et non le
       dossier qu'il recouvre. */
    if (chdir(rootfs) == -1) {
        perror("bocal: chdir (image)");
        return -1;
    }

    /* pivot_root(nouvelle, ancienne) fait de "nouvelle" la racine, et
       range l'ancienne racine (celle de l'hôte) dans le dossier
       "ancienne". Avec "." pour les deux, l'ancienne racine est empilée
       par-dessus la nouvelle, sans dossier temporaire à créer : c'est
       l'astuce décrite dans man 2 pivot_root, et utilisée par runc.
       La glibc n'a pas de fonction pivot_root() : on fait l'appel
       système directement, avec syscall(). */
    if (syscall(SYS_pivot_root, ".", ".") == -1) {
        perror("bocal: pivot_root");
        return -1;
    }

    /* On décroche l'ancienne racine, empilée sur "." : il ne reste plus
       que l'image, et aucun fichier de l'hôte n'est accessible.
       MNT_DETACH la détache même si elle est encore utilisée, par
       exemple par le terminal, qui reste ouvert sur l'hôte. */
    if (umount2(".", MNT_DETACH) == -1) {
        perror("bocal: umount2 (ancienne racine)");
        return -1;
    }

    if (chdir("/") == -1) {
        perror("bocal: chdir");
        return -1;
    }

    /* ps et top lisent /proc. Un /proc monté depuis le conteneur ne
       montre que les processus de son namespace PID.
       NOSUID, NODEV, NOEXEC : les options de sécurité habituelles. */
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == -1) {
        perror("bocal: mount /proc");
        return -1;
    }

    return setup_dev();
}

/*
 * Code exécuté par le processus enfant, déjà à l'intérieur des nouveaux
 * namespaces. Il prépare son environnement, puis se remplace par la
 * commande demandée.
 */
static int child_main(void *arg)
{
    struct child_args *args = arg;
    const struct container_config *cfg = args->cfg;

    /* Si bocal est tué brutalement (kill -9), le noyau tue aussi le
       conteneur : il ne doit jamais lui survivre en tâche de fond. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
        perror("bocal: prctl");
        return 1;
    }

    /* On attend le feu vert du parent, qui doit d'abord nous placer dans
       notre cgroup : la commande ne doit pas démarrer sans ses limites.
       On ferme d'abord notre copie de l'extrémité d'écriture. Sinon, si
       le parent abandonnait, read() attendrait pour toujours. */
    close(args->sync[1]);
    char go;
    if (read(args->sync[0], &go, 1) != 1)
        return 1;   /* tube fermé sans feu vert : le parent a abandonné */
    close(args->sync[0]);

    /* Le nouveau namespace de montage est une copie de celui de l'hôte.
       Sur la plupart des distributions (systemd), ces montages sont
       "partagés" : ce qu'on monterait ici apparaîtrait aussi sur l'hôte.
       On les rend donc privés à ce conteneur, récursivement (MS_REC). */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
        perror("bocal: mount");
        return 1;
    }

    /* Grâce au namespace UTS, ce changement ne touche que le conteneur. */
    if (sethostname(cfg->hostname, strlen(cfg->hostname)) == -1) {
        perror("bocal: sethostname");
        return 1;
    }

    /* Grâce au namespace réseau, le conteneur a sa propre pile réseau,
       vide : on allume juste son interface lo. */
    if (setup_loopback() == -1)
        return 1;

    /* Le dossier de l'image devient la racine du conteneur. */
    if (enter_rootfs(cfg->rootfs) == -1)
        return 1;

    /* execvp() remplace le programme du processus sans changer son PID :
       la commande, cherchée dans l'image, devient donc le PID 1 du
       conteneur. */
    execvp(cfg->argv[0], cfg->argv);

    /* On n'arrive ici que si execvp() a échoué. */
    int err = errno;
    fprintf(stderr, "bocal: impossible de lancer « %s » : %s\n",
            cfg->argv[0], strerror(err));
    /* Mêmes codes que le shell : 127 = introuvable, 126 = non exécutable. */
    return err == ENOENT ? 127 : 126;
}

/*
 * Appelée quand bocal reçoit SIGINT (Ctrl+C), SIGTERM ou SIGHUP. Le PID 1
 * du conteneur ignorerait ces signaux : on le tue donc avec SIGKILL.
 * bocal, lui, continue, ce qui lui laisse le temps de supprimer le cgroup.
 */
static void on_signal(int sig)
{
    received_signal = sig;
    /* Garde-fou : kill(0) ou kill(-1) viseraient bien d'autres processus. */
    if (container_pid > 0)
        kill(container_pid, SIGKILL);
}

/*
 * Crée l'enfant avec clone(), dans de nouveaux namespaces. Renvoie son
 * PID (vu de l'hôte), ou -1 en cas d'erreur.
 */
static pid_t start_child(struct child_args *args)
{
    /* La pile de l'enfant : une zone mémoire que l'on réserve nous-mêmes. */
    char *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        perror("bocal: mmap");
        return -1;
    }

    /* clone() fonctionne comme fork(), mais crée l'enfant directement dans
       les nouveaux namespaces demandés. La pile grandit vers le bas : on
       passe donc l'adresse de sa fin. SIGCHLD : on sera prévenu de la fin
       de l'enfant, comme avec fork(), ce qui permet d'utiliser waitpid(). */
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET |
                SIGCHLD;
    pid_t pid = clone(child_main, stack + STACK_SIZE, flags, args);
    int err = errno;

    /* Sans CLONE_VM, l'enfant a sa propre copie de la mémoire, pile
       comprise : le parent peut libérer la sienne tout de suite. */
    munmap(stack, STACK_SIZE);

    if (pid == -1) {
        fprintf(stderr, "bocal: clone: %s\n", strerror(err));
        if (err == EPERM)
            fprintf(stderr, "bocal: il faut être root pour créer des "
                            "namespaces, relance la commande avec sudo\n");
    }
    return pid;
}

/*
 * Lance le conteneur, le place dans son cgroup (s'il en a un), puis
 * attend qu'il se termine. Renvoie comme container_run().
 */
static int run_child(const struct container_config *cfg, const char *cgroup)
{
    struct child_args args = { .cfg = cfg };

    /* Le tube de synchronisation. O_CLOEXEC : il se fermera tout seul au
       moment d'execvp(), la commande n'en hérite pas. */
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

    /* Le conteneur entre dans son cgroup, puis on lui donne le feu vert
       en écrivant un octet dans le tube. En cas d'échec, on ferme le tube
       sans rien écrire : l'enfant abandonne. */
    bool ok = (cgroup == NULL || cgroup_add(cgroup, pid) == 0) &&
              write(args.sync[1], "x", 1) == 1;

    /* On ne ferme notre extrémité de lecture qu'après avoir écrit : tant
       qu'elle est ouverte, write() ne peut pas déclencher SIGPIPE (qui
       tuerait bocal), même si l'enfant était déjà mort. */
    close(args.sync[0]);
    close(args.sync[1]);

    /* Ctrl+C ou kill sur bocal : on arrête le conteneur (voir on_signal). */
    container_pid = pid;
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* Vu de l'hôte, le conteneur a un PID ordinaire (pid). Vu de
       l'intérieur, c'est le PID 1. On attend qu'il se termine ; si un
       signal interrompt l'attente (EINTR), on la reprend. */
    int status;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w == -1 && errno == EINTR);

    /* Le conteneur est mort : son PID peut désormais être réutilisé par
       un autre processus, le gestionnaire de signal doit l'oublier. */
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
    /* Le cgroup n'est créé que si une limite est demandée. */
    char cgroup[PATH_MAX];
    bool limited = cfg->memory > 0 || cfg->cpus > 0;
    if (limited &&
        cgroup_create(cgroup, sizeof cgroup, cfg->memory, cfg->cpus) == -1)
        return -1;

    int ret = run_child(cfg, limited ? cgroup : NULL);

    /* Le conteneur est terminé, son cgroup est vide : on le supprime. */
    if (limited)
        cgroup_remove(cgroup);
    return ret;
}
