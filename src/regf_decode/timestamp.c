/*
 * function to convert Windows FILETIME to ISO format
 * 
 * Copyright (c) 2026, Akira Yan <yan.akira@gmail.com>
 *
 * Refer to AUTHORS for acknowledgements.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include "timestamp.h"


/**
 * Converts a 64-bit FILETIME (100ns intervals since 1601-01-01)
 * to an ISO 8601 string: YYYY-MM-DDTHH:MM:SS.xxxxxxxZ
 * Make sure the buffer at least have 32 bytes
 */
void format_filetime(
    uint64_t filetime, 
    char* buffer, 
    size_t buffer_size) 
{
    // 1. Number of 100ns intervals in a second
    const uint64_t TICKS_PER_SEC = 10000000ULL;
    
    // 2. Seconds between 1601-01-01 and 1970-01-01
    const uint64_t EPOCH_DIFF = 11644473600ULL;

    // Calculate total seconds and the remaining "ticks" (100ns units)
    uint64_t total_seconds = filetime / TICKS_PER_SEC;
    uint32_t remainder_ticks = (uint32_t)(filetime % TICKS_PER_SEC);
    
    // Adjust to Unix Epoch (1970)
    time_t unix_time = (time_t)(total_seconds - EPOCH_DIFF);

    // Convert to UTC struct tm
    struct tm *utc_time = gmtime(&unix_time);

    // Format the main date/time part
    // %07u ensures we show all 7 digits of the 100ns precision
    strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%S", utc_time);
    
    // Append the sub-second precision and the 'Z' suffix
    char sub_seconds[12];
    snprintf(sub_seconds, sizeof(sub_seconds), ".%07uZ", remainder_ticks);
    
    // Concatenate
    size_t len = 0;
    while(buffer[len] != '\0') len++;
    for(int i = 0; sub_seconds[i] != '\0' && len < buffer_size - 1; i++) {
        buffer[len++] = sub_seconds[i];
    }
    buffer[len] = '\0';
}

uint64_t read_filetime(const uint8_t *p)
{                       
    uint64_t lo = *(const uint32_t *)p;
    uint64_t hi = *(const uint32_t *)(p + 4);
    return (hi << 32) | lo;
}

/*
 * FILETIME -> ISO 8601 文字列 (100ナノ秒精度)
 * 出力例: 2025-12-25T01:02:03.1234567
 */
void filetime_to_str(uint64_t ft, char *buf, size_t buflen)
{
    if (ft == 0) { snprintf(buf, buflen, "N/A"); return; }
    uint32_t  subsec   = (uint32_t)(ft % 10000000ULL);  /* 100ns単位, 0-9999999 */
    uint64_t  unix_sec = ft / 10000000ULL - 11644473600ULL;
    time_t    t        = (time_t)unix_sec;
    struct tm *tm      = gmtime(&t);
    if (!tm) { snprintf(buf, buflen, "Invalid"); return; }
    char base[32];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", tm); 
    snprintf(buf, buflen, "%s.%07u", base, subsec);
}


// sample to use the above function
// int main() {
//       // Example: A timestamp from early 2026
//       uint64_t example_ft = 134114411391234567ULL; 
//       char timestamp[32];
//   
//       format_filetime(example_ft, timestamp, sizeof(timestamp));
//       printf("%s\n", timestamp);
//   
//       return 0;
// }
