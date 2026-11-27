# Bocal

Un moteur de conteneurs minimal écrit en C, pour comprendre ce que fait Docker sous le capot.

Un conteneur n'est pas une machine virtuelle : c'est un processus Linux ordinaire que le noyau isole (namespaces) et limite (cgroups). Bocal utilise directement ces mécanismes, avec les appels système de Linux et sans aucune dépendance.

## Prérequis

- Linux, avec les droits root (`sudo`). Sous Windows, WSL2 convient ; sous macOS, il faut une machine virtuelle Linux.
- Les cgroups v2, pour `--memory` et `--cpus` : `stat -fc %T /sys/fs/cgroup` doit afficher `cgroup2fs`. C'est le cas sur les distributions récentes et sous WSL2.
- `gcc` (ou `clang`), `make` et `curl`. Sur Debian ou Ubuntu : `sudo apt install build-essential curl`.

## Compiler et tester

```sh
make
make test
```

La première fois, `make test` télécharge l'image Alpine 3.24 (environ 3,5 Mo) dans le dossier `alpine/`, après avoir vérifié son empreinte SHA-512. On peut aussi la télécharger seule avec `make alpine`, et la retélécharger avec `sudo rm -rf alpine && make alpine`.

Les 26 tests sont rangés par semaine. Certains vérifient l'effet réel des limites : un programme trop gourmand doit être tué, et un calcul limité à un quart de cœur doit être nettement ralenti.

## Utilisation

```sh
sudo ./bocal run [options] <image> <commande> [arguments...]
```

| Option                  | Effet                                          |
|-------------------------|------------------------------------------------|
| `-n`, `--hostname NOM`  | nom d'hôte du conteneur (par défaut : `bocal`) |
| `-m`, `--memory TAILLE` | mémoire maximale, par exemple `64M` ou `1G`    |
| `--cpus NOMBRE`         | nombre de cœurs maximal, par exemple `0.5`     |

Exemple :

```console
$ sudo ./bocal run --hostname demo ./alpine /bin/sh
/ # cat /etc/alpine-release
3.24.4
/ # hostname
demo
/ # ps
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/sh
    4 root      0:00 ps
/ # ping -c 1 1.1.1.1
PING 1.1.1.1 (1.1.1.1): 56 data bytes
ping: sendto: Network unreachable
/ # exit
```

Le shell tourne dans Alpine, il est le PID 1 de son conteneur, ne voit aucun processus de la machine et n'a pas accès au réseau. Le nom d'hôte de la machine, lui, n'a pas changé.

Avec une limite de mémoire, un programme trop gourmand est tué par le noyau :

```console
$ sudo ./bocal run --memory 32M ./alpine sh -c 'x=a; while true; do x=$x$x; done'
$ echo $?
137
```

## Comment ça marche

### Isolation : les namespaces

`bocal run` appelle `clone()`, une variante de `fork()` qui crée le processus enfant directement dans de nouveaux namespaces :

| Drapeau        | Ce que le conteneur obtient                                  |
|----------------|--------------------------------------------------------------|
| `CLONE_NEWPID` | sa propre numérotation des processus : il démarre au PID 1   |
| `CLONE_NEWUTS` | son propre nom d'hôte                                        |
| `CLONE_NEWNS`  | sa propre table de montages                                  |
| `CLONE_NEWNET` | sa propre pile réseau, avec pour seule interface `lo`        |

Ensuite, l'enfant :

1. demande au noyau d'être tué si `bocal` meurt brutalement (`prctl(PR_SET_PDEATHSIG)`), pour qu'aucun conteneur ne reste orphelin ;
2. attend que `bocal` l'ait placé dans son cgroup (voir plus bas) ;
3. rend ses montages privés (`MS_PRIVATE`), sinon ce qu'il monte apparaîtrait aussi sur l'hôte ;
4. change son nom d'hôte ;
5. allume son interface `lo` (127.0.0.1) avec `ioctl()`. Son namespace réseau ne contient rien d'autre : il n'a accès ni au réseau de la machine, ni à Internet ;
6. change de racine (voir ci-dessous) ;
7. se remplace par la commande avec `execvp()`, qui conserve le PID 1.

Le processus `bocal`, lui, attend la fin du conteneur avec `waitpid()` et renvoie son code de sortie.

### Système de fichiers : pivot_root

L'image est un simple dossier qui contient un système Linux complet. Pour qu'elle devienne la racine `/` du conteneur, l'enfant :

1. monte le dossier de l'image sur lui-même (*bind mount*), car `pivot_root()` exige que la nouvelle racine soit un point de montage ;
2. appelle `pivot_root(".", ".")` : l'image devient la racine, et l'ancienne racine (celle de l'hôte) est empilée par-dessus ;
3. décroche l'ancienne racine avec `umount2(".", MNT_DETACH)`, pour que plus aucun fichier de l'hôte ne soit accessible ;
4. monte un `/proc` neuf. `ps` lit `/proc`, et un `/proc` monté depuis le conteneur ne montre que les processus de son namespace PID ;
5. crée un `/dev` minimal : un *tmpfs* (un système de fichiers en mémoire) où `mknod()` crée `null`, `zero`, `full`, `random`, `urandom` et `tty`, plus les raccourcis `/dev/fd`, `/dev/stdin`, `/dev/stdout` et `/dev/stderr`. Le `/dev` de l'hôte, lui, donnerait accès à tous ses périphériques, disques compris.

Contrairement à `chroot()`, qui ne change que le point de départ des chemins, `pivot_root()` retire l'ancienne racine du namespace de montage : impossible d'en sortir en remontant les dossiers.

### Ressources : les cgroups v2

Avec `--memory` ou `--cpus`, `bocal` crée un *cgroup* pour le conteneur : le dossier `/sys/fs/cgroup/bocal-<PID de bocal>`. Ses fichiers règlent des limites que le noyau applique à tous les processus placés dedans, et à leurs descendants.

| Option         | Fichier           | Valeur écrite  | Effet                                          |
|----------------|-------------------|----------------|------------------------------------------------|
| `--memory 64M` | `memory.max`      | `67108864`     | au-delà, le noyau tue un processus du cgroup   |
|                | `memory.swap.max` | `0`            | pas de débordement dans le swap                |
| `--cpus 0.5`   | `cpu.max`         | `50000 100000` | 50 ms de calcul au plus toutes les 100 ms      |

Le conteneur doit être dans son cgroup avant que sa commande démarre, sinon elle pourrait déjà consommer sans limite. `bocal` et l'enfant se synchronisent donc avec un tube : l'enfant attend, bloqué sur `read()`, que `bocal` ait écrit son PID dans `cgroup.procs`, puis lui envoie un octet comme feu vert.

Un conteneur tué pour dépassement de mémoire reçoit `SIGKILL` : `bocal` renvoie alors 137 (128 + 9), comme Docker. À la fin, `bocal` supprime le cgroup avec `rmdir()`. Il le fait aussi après un Ctrl+C ou un `kill` : il intercepte le signal, tue le conteneur avec `SIGKILL` (son PID 1 ignorerait les autres signaux), puis fait le ménage.

## Limites connues

- Le conteneur est root, et c'est le vrai root de la machine : Bocal n'utilise pas de namespace utilisateur, ne retire aucune capacité et ne filtre pas les appels système. Il n'est donc pas fait pour lancer des programmes auxquels on ne fait pas confiance.
- Le conteneur travaille directement dans le dossier `alpine/` : un fichier supprimé dans le conteneur disparaît de l'image.
- Le conteneur n'a aucun accès au réseau.
- Si `bocal` est tué avec `kill -9`, il ne peut pas supprimer son cgroup : un dossier `bocal-<PID>` vide reste alors dans `/sys/fs/cgroup`, et `sudo rmdir` le supprime. Le conteneur, lui, est bien arrêté.

## Pour aller plus loin

Ce que fait Docker et que Bocal ne fait pas :

- **le réseau** : une paire d'interfaces virtuelles (*veth*) relie le conteneur à un pont (*bridge*) sur l'hôte, et du NAT lui donne accès à Internet ;
- **le namespace utilisateur** : le conteneur est root chez lui, mais un simple utilisateur sur l'hôte (conteneurs *rootless*) ;
- **les images en couches** : avec overlayfs, l'image reste en lecture seule et chaque conteneur écrit dans sa propre couche ;
- **la sécurité** : retrait des capacités de root, et filtre `seccomp` qui bloque les appels système dangereux ;
- **`docker exec`** : entrer dans un conteneur qui tourne déjà, avec `setns()`.

## Organisation du code

```
src/main.c          ligne de commande (bocal run ...)
src/container.c     création du conteneur : clone(), namespaces, pivot_root, /dev, réseau
src/cgroup.c        limites de ressources avec les cgroups v2
src/*.h             interfaces entre ces fichiers
scripts/alpine.sh   téléchargement de l'image Alpine (make alpine)
tests/run.sh        tests automatiques (make test)
```

## Ressources

- Les pages de manuel ([man7.org](https://man7.org/linux/man-pages/)) : `clone(2)`, `namespaces(7)`, `pid_namespaces(7)`, `mount_namespaces(7)`, `pivot_root(2)`, `mknod(2)`, `netdevice(7)` et `cgroups(7)`.
- La documentation du noyau sur les cgroups v2 : [Control Group v2](https://docs.kernel.org/admin-guide/cgroup-v2.html).
- L'image utilisée : le *minirootfs* d'[Alpine Linux](https://alpinelinux.org/downloads/).
