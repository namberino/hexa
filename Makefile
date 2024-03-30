FLAGS = -Wall -Wextra -pedantic -std=c99

hexa: hexa.c
	gcc $(FLAGS) $< -o $@
