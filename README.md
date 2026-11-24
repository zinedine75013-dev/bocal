# Bocal

Un moteur de conteneurs minimal écrit en C, pour comprendre ce que fait Docker sous le capot.

Un conteneur n'est pas une machine virtuelle : c'est un processus Linux ordinaire que le noyau isole (namespaces) et limite (cgroups). Bocal utilise directement ces mécanismes, avec les appels système de Linux et sans aucune dépendance.

## Avancement

- [x] Semaine 1 : isolation des processus (namespaces PID, UTS et montage)
- [ ] Semaine 2 : système de fichiers Alpine avec `pivot_root`, et un `/proc` propre au conteneur
- [ ] Semaine 3 : limites mémoire et CPU avec les cgroups v2
- [ ] Semaine 4 : ligne de commande complète, tests et screencast

## Prérequis

- Linux, avec les droits root (`sudo`). Sous Windows, WSL2 convient ; sous macOS, il faut une machine virtuelle Linux.
- `gcc` (ou `clang`) et `make`. Sur Debian ou Ubuntu : `sudo apt install build-essential`.

## Compiler et tester

```sh
make
make test
```

## Utilisation

```sh
sudo ./bocal run [--hostname NOM] <commande> [arguments...]
```

Exemple :

```console
$ sudo ./bocal run --hostname demo /bin/sh
# echo $$
1
# hostname
demo
# exit
$ hostname
mon-pc
```

Dans le conteneur, le shell est le PID 1 et a son propre nom d'hôte. Celui de la machine n'a pas changé.

## Comment ça marche

`bocal run` appelle `clone()`, une variante de `fork()` qui crée le processus enfant directement dans de nouveaux namespaces :

| Drapeau        | Ce que le conteneur obtient                                  |
|----------------|--------------------------------------------------------------|
| `CLONE_NEWPID` | sa propre numérotation des processus : il démarre au PID 1   |
| `CLONE_NEWUTS` | son propre nom d'hôte                                        |
| `CLONE_NEWNS`  | sa propre table de montages                                  |

Ensuite, l'enfant :

1. demande au noyau d'être tué si `bocal` meurt (`prctl(PR_SET_PDEATHSIG)`), pour qu'aucun conteneur ne reste orphelin ;
2. rend ses montages privés (`MS_PRIVATE`), sinon ce qu'il monte apparaîtrait aussi sur l'hôte ;
3. change son nom d'hôte, puis se remplace par la commande avec `execvp()`, qui conserve le PID 1.

Le processus `bocal`, lui, attend la fin du conteneur avec `waitpid()` et renvoie son code de sortie. Un Ctrl+C tue `bocal`, et donc tout le conteneur.

## Limites connues

- Dans le conteneur, `ps` affiche encore tous les processus de la machine. Le namespace PID fonctionne bien (`echo $$` affiche 1), mais `ps` lit `/proc`, qui est toujours celui de l'hôte. C'est l'objet de la semaine 2.
- En quittant un shell interactif, `sh` peut afficher « Cannot set tty process group ». Le shell essaie de rendre le terminal au groupe de processus de `bocal`, qu'il ne voit pas puisque celui-ci est hors de son namespace PID. C'est sans conséquence. Docker évite ce cas en donnant au conteneur son propre terminal virtuel (option `-t`) : une piste de bonus pour la semaine 4.

## Organisation du code

```
src/main.c        ligne de commande (bocal run ...)
src/container.c   création du conteneur : clone() et préparation de l'enfant
src/container.h   interface entre les deux
tests/run.sh      tests automatiques (make test)
```
