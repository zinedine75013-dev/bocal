#!/bin/sh
# Tests automatiques de Bocal. Lancement : make test

# Créer des namespaces demande les droits root.
[ "$(id -u)" -eq 0 ] || exec sudo sh "$0" "$@"

cd "$(dirname "$0")/.." || exit 1
BOCAL=./bocal
pass=0
fail=0

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
      1 "$($BOCAL run sh -c 'echo $$')"

check "nom d'hôte par défaut" \
      bocal "$($BOCAL run uname -n)"

check "nom d'hôte choisi avec --hostname" \
      test "$($BOCAL run --hostname test uname -n)"

host_before=$(uname -n)
$BOCAL run --hostname autre true
check "le nom d'hôte de la machine ne change pas" \
      "$host_before" "$(uname -n)"

dir=$(mktemp -d)
$BOCAL run sh -c "mount -t tmpfs none '$dir' && touch '$dir/secret'"
if [ -e "$dir/secret" ]; then visible=oui; else visible=non; fi
check "un montage fait dans le conteneur est invisible depuis l'hôte" \
      non "$visible"
umount "$dir" 2>/dev/null
rmdir "$dir"

$BOCAL run sh -c 'exit 42'
check "le code de sortie de la commande est transmis" 42 "$?"

$BOCAL run commande-qui-nexiste-pas 2>/dev/null
check "commande introuvable : code 127, comme le shell" 127 "$?"

echo
echo "$pass réussi(s), $fail échec(s)"
[ "$fail" -eq 0 ]
