# sref version
VERSION = 0.0

# Customize below to fit your system

# Install paths
PREFIX := /usr/local
MANPREFIX := $(PREFIX)/share/man

# Depencies includes and libs
INCS := `pkg-config --cflags x11 glx`
LIBS := `pkg-config --libs x11 glx`

# Flags
CFLAGS ?= -std=c99 -pedantic -march=native -D_XOPEN_SOURCE=500 -D_POSIX_C_SOURCE=200112L
CFLAGS += -Wall -Wextra -O2 -g
CFLAGS += $(INCS) -DVERSION=\"$(VERSION)\"

LDFLAGS += $(LIBS) -ldl -lm
