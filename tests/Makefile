# Тест протокола CP2112 на ПК: USB и I2C заменены заглушками
CFLAGS = -Wall -Wextra -Werror -I stub -I ../src

test_cp2112: test_cp2112.c ../src/cp2112.c
	$(CC) $(CFLAGS) $^ -o $@

run: test_cp2112
	./test_cp2112

.PHONY: run
