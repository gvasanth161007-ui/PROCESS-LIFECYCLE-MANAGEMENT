plms: server.c
	gcc -O2 -Wall -o plms server.c
run: plms
	./plms 8080
