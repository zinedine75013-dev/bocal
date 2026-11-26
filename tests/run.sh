#!/bin/sh
# Tests automatiques de Bocal. Lancement : make test

# Créer des namespaces demande les droits root.
[ "$(id -u)" -eq 0 ] || exec sudo sh "$0" "$@"

cd "$(dirname "$0")/.." || exit 1
BOCAL=./bocal
IMG=./alpine
pass=0
fail=0

if [ ! -d "$IMG" ]; then
    echo "Image introuvable : lance d'abord make alpine"
    exit 1
fi

# check "description" "valeur attendue" "valeur obtenue"
check() {
    if [ "$2" = "$3" ]; then
        printf '  ok     %s\n' "$1"
        pass=$((pass + 1))
    else
        printf '  ÉCHEC  %s (attendu : %s, obtenu : %s)\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

echo "Semaine 1 : isolation"

check "la commande est le PID 1 du conteneur" \
      1 "$($BOCAL run $IMG sh -c 'echo $$')"

check "nom d'hôte par défaut" \
      bocal "$($BOCAL run $IMG uname -n)"

check "nom d'hôte choisi avec --hostname" \
      test "$($BOCAL run --hostname test $IMG uname -n)"

host_before=$(uname -n)
$BOCAL run --hostname autre $IMG true
check "le nom d'hôte de la machine ne change pas" \
      "$host_before" "$(uname -n)"

if $BOCAL run $IMG sh -c 'mount -t tmpfs none /mnt && touch /mnt/secret' &&
   [ ! -e "$IMG/mnt/secret" ]; then invisible=oui; else invisible=non; fi
check "un montage fait dans le conteneur reste invisible depuis l'hôte" \
      oui "$invisible"

$BOCAL run $IMG sh -c 'exit 42'
check "le code de sortie de la commande est transmis" 42 "$?"

$BOCAL run $IMG commande-qui-nexiste-pas 2>/dev/null
check "commande introuvable : code 127, comme le shell" 127 "$?"

echo
echo "Semaine 2 : système de fichiers"

check "la racine du conteneur est l'image Alpine" \
      "$(cat $IMG/etc/alpine-release)" \
      "$($BOCAL run $IMG cat /etc/alpine-release)"

if $BOCAL run $IMG ls "$PWD/Makefile" >/dev/null 2>&1; then vu=oui; else vu=non; fi
check "les fichiers de l'hôte sont inaccessibles" non "$vu"

# Si l'ancienne racine n'était pas démontée, les montages de l'hôte
# apparaîtraient aussi dans cette liste.
check "le conteneur ne voit que ses montages (/, /proc et /dev)" \
      3 "$($BOCAL run $IMG cat /proc/mounts | wc -l)"

# Sortie de ps : une ligne d'en-tête, puis une ligne par processus. Le
# seul processus visible doit être ps lui-même.
check "ps ne voit que les processus du conteneur" \
      1 "$($BOCAL run $IMG ps | tail -n +2 | wc -l)"

echo
echo "Semaine 3 : limites avec les cgroups v2"

$BOCAL run --memory 64X $IMG true 2>/dev/null
check "--memory refuse une taille invalide" 2 "$?"

$BOCAL run --cpus 0 $IMG true 2>/dev/null
check "--cpus refuse un nombre de cœurs invalide" 2 "$?"

# /proc/self/cgroup donne le cgroup du processus qui le lit : la commande
# doit être dans le sien dès son démarrage.
if $BOCAL run --memory 64M $IMG cat /proc/self/cgroup | grep -q '^0::/bocal-'
then dedans=oui; else dedans=non; fi
check "la commande démarre déjà dans son cgroup" oui "$dedans"

# Pendant que le conteneur tourne, on lit ses limites depuis l'hôte. Son
# cgroup porte le PID de bocal, lancé en arrière-plan ($!).
$BOCAL run --memory 64M --cpus 0.5 $IMG sleep 2 &
cg=/sys/fs/cgroup/bocal-$!
sleep 1
check "--memory 64M : memory.max vaut 67108864 octets (64 × 1024 × 1024)" \
      67108864 "$(cat "$cg/memory.max" 2>/dev/null)"
check "--cpus 0.5 : cpu.max vaut « 50000 100000 »" \
      "50000 100000" "$(cat "$cg/cpu.max" 2>/dev/null)"
wait
if [ -d "$cg" ]; then reste=oui; else reste=non; fi
check "le cgroup est supprimé à la fin du conteneur" non "$reste"

# Une chaîne qui double de taille à chaque tour dépasse vite la limite :
# le noyau tue alors le programme avec SIGKILL, d'où le code 128 + 9.
# Sans limite, la boucle s'arrêterait d'elle-même à 256 Mo.
GOURMAND='x=a; while [ ${#x} -lt 268435456 ]; do x=$x$x; done'
$BOCAL run --memory 32M $IMG sh -c "$GOURMAND"
check "--memory 32M : un programme trop gourmand est tué (code 137)" 137 "$?"

$BOCAL run --memory 32M $IMG true
check "--memory 32M : une commande légère fonctionne" 0 "$?"

# Le même calcul, sans limite puis avec un quart de cœur : il doit être
# nettement plus lent (environ 4 fois).
CALCUL='i=0; while [ $i -lt 300000 ]; do i=$((i + 1)); done'
t0=$(date +%s%N)
$BOCAL run $IMG sh -c "$CALCUL"
t1=$(date +%s%N)
$BOCAL run --cpus 0.25 $IMG sh -c "$CALCUL"
t2=$(date +%s%N)
libre=$(( (t1 - t0) / 1000000 ))
limite=$(( (t2 - t1) / 1000000 ))
if [ "$limite" -gt $((2 * libre)) ]; then ralenti=oui; else ralenti=non; fi
check "--cpus 0.25 : le même calcul est plus lent ($libre ms, puis $limite ms)" \
      oui "$ralenti"

echo
echo "Semaine 4 : /dev et réseau"

check "/dev contient les fichiers spéciaux de base" oui \
      "$($BOCAL run $IMG sh -c '[ -c /dev/null ] && [ -c /dev/zero ] &&
                                [ -c /dev/urandom ] && [ -c /dev/tty ] && echo oui')"

check "/dev/urandom fournit des octets aléatoires" 16 \
      "$($BOCAL run $IMG sh -c 'head -c 16 /dev/urandom | wc -c' | tr -d ' ')"

# Dans un script, sh branche l'entrée d'une tâche de fond sur /dev/null :
# sans lui, il affiche une erreur.
check "une tâche de fond (&) se lance sans erreur dans un script" ok \
      "$($BOCAL run $IMG sh -c 'sleep 0 & wait; echo ok' 2>&1)"

if [ -z "$(ls -A $IMG/dev)" ]; then vide=oui; else vide=non; fi
check "le /dev du conteneur n'est pas écrit dans l'image" oui "$vide"

# /proc/net/dev : deux lignes d'en-tête, puis une ligne par interface.
# paste les met bout à bout, séparées par des virgules.
check "le réseau du conteneur n'a que l'interface lo" lo \
      "$($BOCAL run $IMG sh -c 'tail -n +3 /proc/net/dev | cut -d: -f1' |
         tr -d ' ' | paste -sd, -)"

if $BOCAL run $IMG ping -c 1 -W 1 127.0.0.1 >/dev/null 2>&1
then joint=oui; else joint=non; fi
check "lo est allumée : le conteneur peut se joindre lui-même" oui "$joint"

echo
echo "$pass réussi(s), $fail échec(s)"
[ "$fail" -eq 0 ]
