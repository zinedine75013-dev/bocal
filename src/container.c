/*
 * container.c : création du conteneur.
 *
 * Un conteneur n'est qu'un processus Linux ordinaire, lancé dans de
 * nouveaux "namespaces" : des vues privées sur certaines ressources du
 * système. Bocal en utilise trois :
 *
 *   CLONE_NEWPID  numéros de processus : le conteneur démarre au PID 1
 *   CLONE_NEWUTS  nom d'hôte : le conteneur peut changer le sien
 *   CLONE_NEWNS   points de montage : ce qu'il monte reste chez lui
 *
 * Grâce à son namespace de montage, le conteneur peut ensuite changer de
 * racine : le dossier d'une image (Alpine) devient son "/", et il ne voit
 * plus aucun fichier de l'hôte.
 */
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "container.h"

/* Taille de la pile du processus enfant : clone() ne la crée pas pour nous. */
#define STACK_SIZE (1024 * 1024)

/*
 * Fait du dossier de l'image la racine "/" du conteneur, puis lui monte un
 * /proc neuf. Appelée par l'enfant, déjà dans ses nouveaux namespaces.
 * Renvoie 0, ou -1 en cas d'erreur.
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

    return 0;
}

/*
 * Code exécuté par le processus enfant, déjà à l'intérieur des nouveaux
 * namespaces. Il prépare son environnement, puis se remplace par la
 * commande demandée.
 */
static int child_main(void *arg)
{
    const struct container_config *cfg = arg;

    /* Si bocal meurt (Ctrl+C, kill...), le noyau tue aussi le conteneur :
       il ne doit jamais lui survivre en tâche de fond. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
        perror("bocal: prctl");
        return 1;
    }

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

int container_run(const struct container_config *cfg)
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
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_main, stack + STACK_SIZE, flags, (void *)cfg);
    if (pid == -1) {
        int err = errno;
        perror("bocal: clone");
        if (err == EPERM)
            fprintf(stderr, "bocal: il faut être root pour créer des "
                            "namespaces, relance la commande avec sudo\n");
        munmap(stack, STACK_SIZE);
        return -1;
    }

    /* Vu de l'hôte, le conteneur a un PID ordinaire (pid). Vu de
       l'intérieur, c'est le PID 1. On attend qu'il se termine. */
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            perror("bocal: waitpid");
            munmap(stack, STACK_SIZE);
            return -1;
        }
    }
    munmap(stack, STACK_SIZE);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}
