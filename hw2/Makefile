CC       = gcc
CFLAGS   = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -pthread
INCLUDES = -I.
BUILD    = build
TARGET   = $(BUILD)/line_sim
SOURCES  = src/main.c src/queue.c src/process.c
OBJECTS  = $(SOURCES:src/%.c=$(BUILD)/%.o)
DEPS     = $(OBJECTS:.o=.d)

SEED ?= #seed необязателен
DELAY ?= #мс на единицу модельного времени, необязательно (нужен SEED)

all: $(TARGET)
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

run: $(TARGET)
	./$(TARGET) $(SEED) $(DELAY)

-include $(DEPS)
.PHONY: all clean run
