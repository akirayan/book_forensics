CC      := gcc
# _POSIX_C_SOURCE: Linux の -std=c11 では strdup/getline 等の POSIX 関数が隠れるため明示的に有効化
CFLAGS  := -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
# iconv: macOS(Darwin) は libiconv が別ライブラリなので -liconv が必要。
# Linux(glibc) は iconv が libc に含まれるため不要（-liconv は見つからずリンクエラーになる）。
LDFLAGS :=
ifeq ($(shell uname -s),Darwin)
  LDFLAGS += -liconv
endif

TARGET  := evtx_decode
SRCS    := main.c hex_dump.c timestamp.c evtx_file.c evtx_chunk.c evtx_record.c evtx_binxml.c utf16le.c evtx_xmltree.c evtx_output.c stack.c
OBJS    := $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c hex_dump.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
