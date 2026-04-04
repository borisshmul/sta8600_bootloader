CC      = gcc

# Detect CentOS / RHEL vs Debian / Ubuntu by checking which OpenSSL
# library directory exists.  CentOS uses /usr/lib64 on x86-64.
OPENSSL_LIB_DIR := $(shell \
    if [ -d /usr/lib64/openssl ] || [ -f /usr/lib64/libcrypto.so ]; then \
        echo /usr/lib64; \
    else \
        echo /usr/lib; \
    fi)

# _DEFAULT_SOURCE exposes cfmakeraw() and ETIMEDOUT on CentOS/RHEL glibc.
# It is a no-op on Debian/Ubuntu where these are already visible.
CFLAGS  = -Wall -Wextra -O2 -g \
          -D_DEFAULT_SOURCE \
          -DHAVE_OPENSSL

LDFLAGS = -L$(OPENSSL_LIB_DIR) -lcrypto

TARGET  = sta_flash
SRCS    = main.c protocol.c secure.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean info

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

# Show detected paths (useful when troubleshooting on CentOS)
info:
	@echo "OpenSSL lib dir : $(OPENSSL_LIB_DIR)"
	@echo "Compiler        : $(CC)"
	@pkg-config --modversion openssl 2>/dev/null || openssl version

# Generate a development keypair into ../keys/
keygen: $(TARGET)
	mkdir -p ../keys
	./$(TARGET) --keygen
