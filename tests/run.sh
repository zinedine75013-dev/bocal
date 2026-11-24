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
check "le conteneur ne voit que ses montages (/ et /proc)" \
      2 "$($BOCAL run $IMG cat /proc/mounts | wc -l)"

# Sortie de ps : une ligne d'en-tête, puis une ligne par processus. Le
# seul processus visible doit être ps lui-même.
check "ps ne voit que les processus du conteneur" \
      1 "$($BOCAL run $IMG ps | tail -n +2 | wc -l)"

echo
echo "$pass réussi(s), $fail échec(s)"
[ "$fail" -eq 0 ]
