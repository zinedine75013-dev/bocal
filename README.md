# Bocal

Un moteur de conteneurs minimal écrit en C, pour comprendre ce que fait Docker sous le capot.

Un conteneur n'est pas une machine virtuelle : c'est un processus Linux ordinaire que le noyau isole (namespaces) et limite (cgroups). Bocal utilise directement ces mécanismes, avec les appels système de Linux et sans aucune dépendance.

## Avancement

- [x] Semaine 1 : isolation des processus (namespaces PID, UTS et montage)
- [x] Semaine 2 : système de fichiers Alpine avec `pivot_root`, et un `/proc` propre au conteneur
- [ ] Semaine 3 : limites mémoire et CPU avec les cgroups v2
- [ ] Semaine 4 : ligne de commande complète, tests et screencast

## Prérequis

- Linux, avec les droits root (`sudo`). Sous Windows, WSL2 convient ; sous macOS, il faut une machine virtuelle Linux.
- `gcc` (ou `clang`), `make` et `curl`. Sur Debian ou Ubuntu : `sudo apt install build-essential curl`.

## Compiler et tester

```sh
make
make test
```

La première fois, `make test` télécharge l'image Alpine 3.24 (environ 3,5 Mo) dans le dossier `alpine/`, après avoir vérifié son empreinte SHA-512. On peut aussi la télécharger seule avec `make alpine`, et la retélécharger avec `sudo rm -rf alpine && make alpine`.

## Utilisation

```sh
sudo ./bocal run [--hostname NOM] <image> <commande> [arguments...]
```

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

## Comment ça marche

### Isolation : les namespaces

`bocal run` appelle `clone()`, une variante de `fork()` qui crée le processus enfant directement dans de nouveaux namespaces :

| Drapeau        | Ce que le conteneur obtient                                  |
|----------------|--------------------------------------------------------------|
| `CLONE_NEWPID` | sa propre numérotation des processus : il démarre au PID 1   |
| `CLONE_NEWUTS` | son propre nom d'hôte                                        |
| `CLONE_NEWNS`  | sa propre table de montages                                  |

Ensuite, l'enfant :

1. demande au noyau d'être tué si `bocal` meurt (`prctl(PR_SET_PDEATHSIG)`), pour qu'aucun conteneur ne reste orphelin ;
2. rend ses montages privés (`MS_PRIVATE`), sinon ce qu'il monte apparaîtrait aussi sur l'hôte ;
3. change son nom d'hôte ;
4. change de racine (voir ci-dessous) ;
5. se remplace par la commande avec `execvp()`, qui conserve le PID 1.

Le processus `bocal`, lui, attend la fin du conteneur avec `waitpid()` et renvoie son code de sortie. Un Ctrl+C tue `bocal`, et donc tout le conteneur.

### Système de fichiers : pivot_root

L'image est un simple dossier qui contient un système Linux complet. Pour qu'elle devienne la racine `/` du conteneur, l'enfant :

1. monte le dossier de l'image sur lui-même (*bind mount*), car `pivot_root()` exige que la nouvelle racine soit un point de montage ;
2. appelle `pivot_root(".", ".")` : l'image devient la racine, et l'ancienne racine (celle de l'hôte) est empilée par-dessus ;
3. décroche l'ancienne racine avec `umount2(".", MNT_DETACH)`, pour que plus aucun fichier de l'hôte ne soit accessible ;
4. monte un `/proc` neuf. `ps` lit `/proc`, et un `/proc` monté depuis le conteneur ne montre que les processus de son namespace PID.

Contrairement à `chroot()`, qui ne change que le point de départ des chemins, `pivot_root()` retire l'ancienne racine du namespace de montage : impossible d'en sortir en remontant les dossiers.

## Limites connues

- Le conteneur travaille directement dans le dossier `alpine/` : un fichier supprimé dans le conteneur disparaît de l'image. Docker évite ça avec overlayfs, qui pose une couche modifiable propre à chaque conteneur sur une image en lecture seule.
- `/dev` est vide dans le conteneur : il n'y a ni `/dev/null` ni `/dev/zero`.
- Le conteneur partage le réseau de la machine, faute de namespace réseau.

## Organisation du code

```
src/main.c          ligne de commande (bocal run ...)
src/container.c     création du conteneur : clone(), namespaces, pivot_root
src/container.h     interface entre les deux
scripts/alpine.sh   téléchargement de l'image Alpine (make alpine)
tests/run.sh        tests automatiques (make test)
```
