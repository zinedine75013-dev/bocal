/*
 * main.c : ligne de commande de Bocal.
 *
 *   bocal run [options] <commande> [arguments...]
 */
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "container.h"

static void usage(FILE *out)
{
    fprintf(out,
        "Usage : bocal run [options] <commande> [arguments...]\n"
        "\n"
        "Lance <commande> dans un conteneur isolé (nouveaux namespaces PID,\n"
        "UTS et montage). Nécessite les droits root.\n"
        "\n"
        "Options :\n"
        "  -n, --hostname NOM   nom d'hôte du conteneur (par défaut : bocal)\n"
        "  -h, --help           affiche cette aide\n"
        "\n"
        "Exemple :\n"
        "  sudo ./bocal run --hostname demo /bin/sh\n");
}

static int cmd_run(int argc, char **argv)
{
    struct container_config cfg = { .hostname = "bocal" };

    static const struct option options[] = {
        { "hostname", required_argument, NULL, 'n' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL,       0,                 NULL, 0   },
    };

    /* "+" : on s'arrête au premier argument qui n'est pas une option. Dans
       "bocal run sh -c 'ls -l'", le "-c" appartient à sh, pas à bocal.
       ":" : on affiche nous-mêmes les messages d'erreur. */
    int c;
    while ((c = getopt_long(argc, argv, "+:n:h", options, NULL)) != -1) {
        switch (c) {
        case 'n':
            if (strlen(optarg) > HOST_NAME_MAX) {
                fprintf(stderr, "bocal: nom d'hôte trop long "
                                "(%d caractères maximum)\n", HOST_NAME_MAX);
                return 2;
            }
            cfg.hostname = optarg;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case ':':
            fprintf(stderr, "bocal: l'option %s attend une valeur\n",
                    argv[optind - 1]);
            return 2;
        default:
            fprintf(stderr, "bocal: option inconnue : %s\n",
                    argv[optind - 1]);
            fprintf(stderr, "Pour l'aide : bocal run --help\n");
            return 2;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "bocal: il manque la commande à lancer\n\n");
        usage(stderr);
        return 2;
    }

    cfg.argv = &argv[optind];
    int ret = container_run(&cfg);
    return ret < 0 ? 1 : ret;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc - 1, argv + 1);

    fprintf(stderr, "bocal: commande inconnue : %s\n\n", argv[1]);
    usage(stderr);
    return 2;
}
