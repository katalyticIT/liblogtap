
##---- Makefile for liblogtap -------------------------

# Compiler und Flags
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -fPIC
LDFLAGS = -shared -ldl -lpthread

# source and target files
SOURCE = src/liblogtap.c
TARGET = local/liblogtap.so

# default target
all: $(TARGET)

# rule for building the library
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build successfully: $(TARGET)"

# clean up old build artefacts
clean:
	rm -f $(TARGET)
	@echo "Removed file: $(TARGET)"

# Avoids conflicts with files called "all" or "clean"
.PHONY: all clean

