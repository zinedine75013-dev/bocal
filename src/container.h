#ifndef BOCAL_CONTAINER_H
#define BOCAL_CONTAINER_H

/* Tout ce qu'il faut savoir pour lancer un conteneur. */
struct container_config {
    const char *rootfs;        /* dossier de l'image, qui deviendra "/" */
    const char *hostname;      /* nom d'hôte vu depuis le conteneur */
    unsigned long long memory; /* mémoire maximale en octets (0 : sans limite) */
    double cpus;               /* nombre de cœurs maximal (0 : sans limite) */
    char **argv;               /* commande à lancer, terminée par NULL */
};

/*
 * Lance cfg->argv dans un conteneur et attend qu'il se termine.
 *
 * Renvoie le code de sortie de la commande (128 + n si elle a été tuée
 * par le signal n, comme le fait le shell), ou -1 si le conteneur n'a
 * pas pu être créé.
 */
int container_run(const struct container_config *cfg);

#endif
