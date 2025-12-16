CC ?= gcc-15
X11_INC ?= /opt/X11/include
X11_LIB ?= /opt/X11/lib
CFLAGS = -std=c11 -Wall -Wextra -I./include -I/opt/X11/include -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -g
LDFLAGS = -L$(X11_LIB) -lX11 -pthread -Wl,-rpath,$(X11_LIB)

SRC = $(wildcard src/*.c)
OBJ = $(patsubst src/%.c,build/%.o,$(SRC))
TARGET = myterm

.PHONY: all clean tests

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)


tests: test_x11 test_fork test_pipe test_termios

test_x11: | build
	$(CC) tests/x11_test.c -o build/x11_test -I$(X11_INC) -L$(X11_LIB) -lX11 -pthread -Wl,-rpath,$(X11_LIB)

test_fork: | build
	$(CC) tests/fork_exec_test.c -o build/fork_exec_test -pthread

test_pipe: | build
	$(CC) tests/pipe_test.c -o build/pipe_test -pthread

test_termios: | build
	$(CC) tests/termios_test.c -o build/termios_test -pthread

