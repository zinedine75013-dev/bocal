# Makefile de Bocal

CC     = cc
# _GNU_SOURCE donne accès aux fonctions propres à Linux (clone, sethostname...)
CFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra -O2 -g

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)
HDR = $(wildcard src/*.h)

bocal: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Les tests créent des namespaces : le script se relance avec sudo si besoin.
test: bocal
	sh tests/run.sh

clean:
	rm -f $(OBJ) bocal

.PHONY: test clean
