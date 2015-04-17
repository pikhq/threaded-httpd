LDFLAGS+=-lpthread
CFLAGS+=-std=c99
httpd: main.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lpthread
