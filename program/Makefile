all: ca_spin

ca_spin: ca_spin.c
	gcc -O0 -o ca_spin ca_spin.c -static -lrt

clean:
	rm -f ca_spin

install: all
	cp -f ca_spin ../bin
