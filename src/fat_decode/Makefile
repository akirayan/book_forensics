CC      := gcc
# -std=gnu11: timegm / strdup を macOS・Linux(glibc) の両方で有効化する
# （厳格な -std=c11 だと glibc でこれらが隠れてコンパイルエラーになる）
CFLAGS  := -Wall -Wextra -O2 -std=gnu11
# iconv: macOS(Darwin) は libiconv が別ライブラリなので -liconv が必要。
# Linux(glibc) は iconv が libc に含まれるため不要（-liconv は見つからずリンクエラーになる）。
LDFLAGS :=
ifeq ($(shell uname -s),Darwin)
  LDFLAGS += -liconv
endif

TARGET  := fat_decode
SRCS    := main.c fatlib.c fat32.c exfat.c
OBJS    := $(SRCS:.c=.o)
HDRS    := $(wildcard *.h)

.PHONY: all clean legacy

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

# 旧・単一ファイル版 (出力一致検証と移行期間用、既定ビルドには含めない)。
# 本体は fat_decode に一本化済み。削除は著者判断。
legacy: fat32_decode exfat_decode

fat32_decode: fat32_decode.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

exfat_decode: exfat_decode.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS) fat32_decode exfat_decode
