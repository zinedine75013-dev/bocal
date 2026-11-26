/*
 * main.c : ligne de commande de Bocal.
 *
 *   bocal run [options] <image> <commande> [arguments...]
 */
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "container.h"

static void usage(FILE *out)
{
    fprintf(out,
        "Usage : bocal run [options] <image> <commande> [arguments...]\n"
        "\n"
        "Lance <commande> dans un conteneur isolé (nouveaux namespaces PID,\n"
        "UTS et montage), dont la racine est le dossier <image>.\n"
        "Nécessite les droits root.\n"
        "\n"
        "Options :\n"
        "  -n, --hostname NOM    nom d'hôte du conteneur (par défaut : bocal)\n"
        "  -m, --memory TAILLE   mémoire maximale, par exemple 64M ou 1G\n"
        "      --cpus NOMBRE     nombre de cœurs maximal, par exemple 0.5\n"
        "  -h, --help            affiche cette aide\n"
        "\n"
        "Exemple :\n"
        "  sudo ./bocal run --memory 64M --cpus 0.5 ./alpine /bin/sh\n");
}

/*
 * Lit une taille mémoire : un nombre, suivi éventuellement de K, M ou G
 * (kilo-, méga- ou gigaoctets), par exemple "64M". Renvoie la taille en
 * octets, ou 0 si la valeur est invalide.
 */
static unsigned long long parse_memory(const char *s)
{
    /* strtoull() accepte aussi les espaces et les signes ("-5" deviendrait
       un nombre immense) : on exige donc un chiffre en premier. */
    if (!isdigit((unsigned char)s[0]))
        return 0;

    char *end;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno != 0)
        return 0;

    int shift = 0;   /* 1K = 2^10 octets, 1M = 2^20, 1G = 2^30 */
    switch (*end) {
    case 'k': case 'K': shift = 10; end++; break;
    case 'm': case 'M': shift = 20; end++; break;
    case 'g': case 'G': shift = 30; end++; break;
    }

    /* Rien ne doit suivre, et le décalage ne doit pas déborder. */
    if (*end != '\0' || n > (ULLONG_MAX >> shift))
        return 0;
    return n << shift;
}

/*
 * Lit un nombre de cœurs, par exemple "0.5". Renvoie -1 si la valeur est
 * invalide. Le minimum, 0.01, correspond au plus petit quota accepté par
 * le noyau : 1 ms toutes les 100 ms.
 */
static double parse_cpus(const char *s)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    /* Écrit avec « ! » pour refuser aussi NaN, qui rate toute comparaison. */
    if (errno != 0 || end == s || *end != '\0' || !(v >= 0.01 && v <= 1024))
        return -1;
    return v;
}

static int cmd_run(int argc, char **argv)
{
    struct container_config cfg = { .hostname = "bocal" };

    static const struct option options[] = {
        { "hostname", required_argument, NULL, 'n' },
        { "memory",   required_argument, NULL, 'm' },
        { "cpus",     required_argument, NULL, 'c' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL,       0,                 NULL, 0   },
    };

    /* "+" : on s'arrête au premier argument qui n'est pas une option. Dans
       "bocal run ./alpine sh -c 'ls -l'", le "-c" appartient à sh, pas à
       bocal. ":" : on affiche nous-mêmes les messages d'erreur. --cpus n'a
       pas de forme courte : 'c' n'apparaît donc pas dans cette liste. */
    int c;
    while ((c = getopt_long(argc, argv, "+:n:m:h", options, NULL)) != -1) {
        switch (c) {
        case 'n':
            if (strlen(optarg) > HOST_NAME_MAX) {
                fprintf(stderr, "bocal: nom d'hôte trop long "
                                "(%d caractères maximum)\n", HOST_NAME_MAX);
                return 2;
            }
            cfg.hostname = optarg;
            break;
        case 'm':
            cfg.memory = parse_memory(optarg);
            if (cfg.memory == 0) {
                fprintf(stderr, "bocal: --memory %s : taille invalide "
                                "(exemples : 64M, 1G)\n", optarg);
                return 2;
            }
            break;
        case 'c':
            cfg.cpus = parse_cpus(optarg);
            if (cfg.cpus < 0) {
                fprintf(stderr, "bocal: --cpus %s : il faut un nombre de "
                                "cœurs entre 0.01 et 1024, par exemple 0.5\n",
                        optarg);
                return 2;
            }
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

    if (argc - optind < 2) {
        fprintf(stderr, "bocal: il faut une image et une commande\n\n");
        usage(stderr);
        return 2;
    }

    /* L'image doit être un dossier, par exemple ./alpine (make alpine). */
    struct stat st;
    if (stat(argv[optind], &st) == -1 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "bocal: %s n'est pas un dossier d'image\n"
                        "Pour télécharger Alpine : make alpine\n",
                argv[optind]);
        return 1;
    }

    cfg.rootfs = argv[optind];
    cfg.argv = &argv[optind + 1];
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
