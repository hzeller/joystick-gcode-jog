CFLAGS=-Wall -Wextra -std=c11 -D_XOPEN_SOURCE=700
LDFLAGS=-lm
OBJECTS=machine-jog.o joystick-config.o rumble.o

machine-jog: $(OBJECTS)
	gcc $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f machine-jog $(OBJECTS)

format:
	clang-format -i *.c *.h
