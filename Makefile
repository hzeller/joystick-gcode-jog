CFLAGS=-Wall -std=c99 -D_XOPEN_SOURCE=700
LDFLAGS=-lm
OBJECTS=machine-jog.o joystick-config.o rumble.o

machine-jog: $(OBJECTS)
	gcc $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f machine-jog $(OBJECTS)


