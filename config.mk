# sref version
VERSION = 0.0

# Customize below to fit your system

# Install paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

# Depencies includes and libs
INCS = `pkg-config --cflags x11 gl`
LIBS = -ldl -lm `pkg-config --libs x11 gl`

# Flags
CPPFLAGS += -DVERSION=\"$(VERSION)\"
CFLAGS += $(INCS) $(CPPFLAGS) -Wall -Wextra -O2 -g
LDFLAGS += $(LIBS)
