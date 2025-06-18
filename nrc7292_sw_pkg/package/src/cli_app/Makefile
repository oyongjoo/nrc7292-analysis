# NRC7292 CLI Application Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lnl-3 -lnl-genl-3

TARGET = cli_app
SOURCES = main.c cli_cmd.c cli_netlink.c cli_util.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

install: $(TARGET)
	cp $(TARGET) ~/nrc_pkg/script/

.PHONY: all clean install