CC     = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS=

SERVER = server
CLIENT = client

.PHONY: all clean

all: $(SERVER) $(CLIENT)

$(SERVER): server.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread $(LDFLAGS)

$(CLIENT): client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(SERVER) $(CLIENT)

