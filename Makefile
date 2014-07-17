CFLAGS=-Wall -std=c99 -D_XOPEN_SOURCE=700
LDFLAGS=-lm

machine-jog: machine-jog.c
	gcc $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f machine-jog

