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

#if !defined( TIMESTAMP_H )
#define TIMESTAMP_H

#include <stdio.h>
#include <stdint.h>
#include <time.h>


#if defined( __cplusplus )
extern "C" {
#endif

void format_filetime(
     uint64_t filetime, 
     char* buffer, 
     size_t buffer_size);

#if defined( __cplusplus )
}
#endif

#endif /* !defined( TIMESTAMP_H ) */
