CC ?= gcc
CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDFLAGS :=
TARGET := build/fat12tool
SOURCES := src/main.c src/fat12.c

.PHONY: all clean test sanitize

all: $(TARGET)

$(TARGET): $(SOURCES) include/fat12.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

sanitize: CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS := -fsanitize=address,undefined
sanitize: clean $(TARGET)

# Para la solucion docente. En el codigo base, los tests fallaran hasta completar los TODO.
test: all
	python3 tests/public_tests.py ./$(TARGET)

clean:
	rm -rf build test-output
