FLAGS = -Wall -Wextra -pedantic -std=c99

bin/hexa: hexa.c
	gcc $(FLAGS) $< -o $@
