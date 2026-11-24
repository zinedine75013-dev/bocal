#!/bin/sh
# Télécharge l'image Alpine minimale (« minirootfs », environ 3,5 Mo) et
# l'extrait dans le dossier ./alpine. Lancement : make alpine
set -e

VERSION=3.24.4
BRANCH=v3.24

# En root, tar conserve les propriétaires des fichiers (root), comme dans
# une vraie image.
[ "$(id -u)" -eq 0 ] || exec sudo sh "$0" "$@"
cd "$(dirname "$0")/.."

if [ -d alpine ]; then
    echo "L'image ./alpine existe déjà."
    exit 0
fi

command -v curl >/dev/null || {
    echo "Il faut curl : sudo apt install curl" >&2
    exit 1
}

# Empreintes SHA-512 officielles de l'archive, pour vérifier qu'elle n'a
# été ni abîmée ni modifiée pendant le téléchargement.
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)  SUM=fca636242d782ec9c171338014bea34471b63172868898ba7bfe49a56387a2d8a1940c1602d97e470574aa17309ac79c7bcdddce1460de5454814034ec3b0374 ;;
    aarch64) SUM=aa4a4161d4a97db889c47f85a965602f25725974a981af8a80d4511d1655a05cf6650a8276cf5c9ec39c8fd2070dd762ceeed228cef9e9d86e892070d06348f8 ;;
    *) echo "Architecture non prise en charge : $ARCH" >&2; exit 1 ;;
esac

FILE=alpine-minirootfs-$VERSION-$ARCH.tar.gz
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

# Le site officiel d'Alpine, puis son dépôt GitHub en secours.
for URL in \
    "https://dl-cdn.alpinelinux.org/alpine/$BRANCH/releases/$ARCH/$FILE" \
    "https://raw.githubusercontent.com/alpinelinux/docker-alpine/0c9af76133a2dd9a31dedf3600e423087b29a879/$ARCH/$FILE"
do
    echo "Téléchargement de $URL"
    curl -fL --progress-bar -o "$TMP" "$URL" || continue

    if ! echo "$SUM  $TMP" | sha512sum -c --status; then
        echo "Empreinte SHA-512 incorrecte : fichier rejeté." >&2
        continue
    fi

    rm -rf alpine.tmp
    mkdir alpine.tmp
    tar -xzf "$TMP" -C alpine.tmp
    mv alpine.tmp alpine
    echo "Image prête : ./alpine (Alpine $VERSION)"
    exit 0
done

echo "Impossible de télécharger l'image Alpine." >&2
exit 1
