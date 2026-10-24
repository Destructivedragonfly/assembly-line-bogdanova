CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -O2
INCLUDES = -I.
BUILD    = build
TARGET   = $(BUILD)/line_sim
SOURCES  = src/main.c src/queue.c src/events.c src/process.c
OBJECTS  = $(SOURCES:src/%.c=$(BUILD)/%.o)
DEPS     = $(OBJECTS:.o=.d)

SEED ?= #seed необязателен

all: $(TARGET)
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

run: $(TARGET)
	./$(TARGET) $(SEED)

-include $(DEPS)
.PHONY: all clean run