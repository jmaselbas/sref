# sref version
VERSION = 0.1.2

# Customize below to fit your system

# Install paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

# Depencies includes and libs
INCS = `pkg-config --cflags x11 gl xrender xext`
LIBS = -ldl -lm `pkg-config --libs x11 gl xrender xext`

# Flags
CPPFLAGS += -DVERSION=\"$(VERSION)\" -D_POSIX_C_SOURCE=200809L
CFLAGS += $(INCS) $(CPPFLAGS) -Wall -Wextra -O2 -g
LDFLAGS += $(LIBS)
