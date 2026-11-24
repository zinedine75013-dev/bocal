/*
 * container.c : création du conteneur.
 *
 * Un conteneur n'est qu'un processus Linux ordinaire, lancé dans de
 * nouveaux "namespaces" : des vues privées sur certaines ressources du
 * système. Pour l'instant, Bocal en utilise trois :
 *
 *   CLONE_NEWPID  numéros de processus : le conteneur démarre au PID 1
 *   CLONE_NEWUTS  nom d'hôte : le conteneur peut changer le sien
 *   CLONE_NEWNS   points de montage : ce qu'il monte reste chez lui
 */
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "container.h"

/* Taille de la pile du processus enfant : clone() ne la crée pas pour nous. */
#define STACK_SIZE (1024 * 1024)

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

    /* execvp() remplace le programme du processus sans changer son PID :
       la commande devient donc le PID 1 du conteneur. */
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
