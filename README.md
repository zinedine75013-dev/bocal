# Bocal

Un moteur de conteneurs minimal écrit en C, pour comprendre ce que fait Docker sous le capot.

Un conteneur n'est pas une machine virtuelle : c'est un processus Linux ordinaire que le noyau isole (namespaces) et limite (cgroups). Bocal utilise directement ces mécanismes, avec les appels système de Linux et sans aucune dépendance.

## Avancement

- [x] Semaine 1 : isolation des processus (namespaces PID, UTS et montage)
- [x] Semaine 2 : système de fichiers Alpine avec `pivot_root`, et un `/proc` propre au conteneur
- [x] Semaine 3 : limites mémoire et CPU avec les cgroups v2
- [ ] Semaine 4 : finitions et screencast

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
/ # exit
```

Le shell tourne dans Alpine, il est le PID 1 de son conteneur et ne voit aucun processus de la machine. Le nom d'hôte de la machine, lui, n'a pas changé.

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

Ensuite, l'enfant :

1. demande au noyau d'être tué si `bocal` meurt brutalement (`prctl(PR_SET_PDEATHSIG)`), pour qu'aucun conteneur ne reste orphelin ;
2. attend que `bocal` l'ait placé dans son cgroup (voir plus bas) ;
3. rend ses montages privés (`MS_PRIVATE`), sinon ce qu'il monte apparaîtrait aussi sur l'hôte ;
4. change son nom d'hôte ;
5. change de racine (voir ci-dessous) ;
6. se remplace par la commande avec `execvp()`, qui conserve le PID 1.

Le processus `bocal`, lui, attend la fin du conteneur avec `waitpid()` et renvoie son code de sortie.

### Système de fichiers : pivot_root

L'image est un simple dossier qui contient un système Linux complet. Pour qu'elle devienne la racine `/` du conteneur, l'enfant :

1. monte le dossier de l'image sur lui-même (*bind mount*), car `pivot_root()` exige que la nouvelle racine soit un point de montage ;
2. appelle `pivot_root(".", ".")` : l'image devient la racine, et l'ancienne racine (celle de l'hôte) est empilée par-dessus ;
3. décroche l'ancienne racine avec `umount2(".", MNT_DETACH)`, pour que plus aucun fichier de l'hôte ne soit accessible ;
4. monte un `/proc` neuf. `ps` lit `/proc`, et un `/proc` monté depuis le conteneur ne montre que les processus de son namespace PID.

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

- Le conteneur travaille directement dans le dossier `alpine/` : un fichier supprimé dans le conteneur disparaît de l'image. Docker évite ça avec overlayfs, qui pose une couche modifiable propre à chaque conteneur sur une image en lecture seule.
- `/dev` est vide dans le conteneur : il n'y a ni `/dev/null` ni `/dev/zero`. Les commandes qui en ont besoin échouent, par exemple une tâche de fond (`&`) dans un script `sh -c`.
- Le conteneur partage le réseau de la machine, faute de namespace réseau.
- Si `bocal` est tué avec `kill -9`, il ne peut pas supprimer son cgroup : un dossier `bocal-<PID>` vide reste alors dans `/sys/fs/cgroup`, et `sudo rmdir` le supprime. Le conteneur, lui, est bien arrêté.

## Organisation du code

```
src/main.c          ligne de commande (bocal run ...)
src/container.c     création du conteneur : clone(), namespaces, pivot_root
src/cgroup.c        limites de ressources avec les cgroups v2
src/*.h             interfaces entre ces fichiers
scripts/alpine.sh   téléchargement de l'image Alpine (make alpine)
tests/run.sh        tests automatiques (make test)
```
