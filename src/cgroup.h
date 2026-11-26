#ifndef BOCAL_CGROUP_H
#define BOCAL_CGROUP_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Crée le cgroup du conteneur et y écrit ses limites :
 *   memory : mémoire maximale, en octets (0 : pas de limite)
 *   cpus   : nombre de cœurs, par exemple 0.5 (0 : pas de limite)
 * Le chemin du cgroup est écrit dans path, de taille size.
 * Renvoie 0, ou -1 en cas d'erreur (rien n'est alors créé).
 */
int cgroup_create(char *path, size_t size,
                  unsigned long long memory, double cpus);

/* Place le processus pid dans le cgroup. Renvoie 0, ou -1. */
int cgroup_add(const char *path, pid_t pid);

/* Supprime le cgroup, qui doit être vide. */
void cgroup_remove(const char *path);

#endif
