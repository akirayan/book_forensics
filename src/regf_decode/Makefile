CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -std=c11
# iconv: macOS(Darwin) は libiconv が別ライブラリなので -liconv が必要。
# Linux(glibc) は iconv が libc に含まれるため不要（-liconv は見つからずリンクエラーになる）。
LDFLAGS :=
ifeq ($(shell uname -s),Darwin)
  LDFLAGS += -liconv
endif

TARGET  := regf_decode
SRCS    := main.c regf_file.c timestamp.c regf_hbin.c regf_cell.c regf_keys.c regf_value.c utf16le.c
OBJS    := $(SRCS:.c=.o)
HDRS    := $(wildcard *.h)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
