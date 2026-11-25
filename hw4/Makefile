CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -pedantic -O2 -Iheaders -MMD -MP
LDLIBS_SERVER := -lpthread
SRC_DIR   := src
BUILD_DIR := build
SERVER_SRCS := server.c process.c link.c worker.c observer.c event.c queue.c
CLIENT_SRCS := client.c
SERVER_OBJS := $(addprefix $(BUILD_DIR)/,$(SERVER_SRCS:.c=.o))
CLIENT_OBJS := $(addprefix $(BUILD_DIR)/,$(CLIENT_SRCS:.c=.o))

.PHONY: all clean

all: server client

server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS_SERVER)

client: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) server client

-include $(wildcard $(BUILD_DIR)/*.d)