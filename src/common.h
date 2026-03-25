/*
 * This file is part of pg_what_is_happening.
 * Copyright (C) 2025 toilebril
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 * See top-level LICENSE file.
 */

#if !defined PWH_COMMON_H
#define PWH_COMMON_H

typedef uint8  u8;
typedef uint16 u16;
typedef uint32 u32;
typedef uint64 u64;

typedef int8  i8;
typedef int16 i16;
typedef int32 i32;
typedef int64 i64;

typedef u8 uchar;
typedef i8 ichar;

typedef Size usize;

/* Compiler attributes. */

#if defined __GNUC__ || defined __clang__ || defined __COSMOCC__
#define PWH__HAS_GCC_EXTENSIONS 1
#define pwh__used __attribute__((used))
#define pwh__likely(x) __builtin_expect(!!(x), true)
#define pwh__unlikely(x) __builtin_expect(!!(x), false)
#define pwh__mustuse __attribute__((warn_unused_result))
#define pwh__threadlocal __thread
#define pwh__wontreturn __attribute__((noreturn))
#define pwh__maybeunused __attribute__((unused))
#define pwh__forceinline inline __attribute__((always_inline))
#define pwh__unreachable() __builtin_unreachable()
#define pwh__debugtrap() __builtin_trap()
#define pwh__typeof(t) __typeof__(t)
#define pwh__fallthrough __attribute__((fallthrough))
#else
#define pwh__used /* nothing. */
#define pwh__likely(x) (x)
#define pwh__unlikely(x) (x)
#define pwh__mustuse	 /* nothing. */
#define pwh__threadlocal /* nothing. */
#define pwh__wontreturn	 /* nothing. */
#define pwh__maybeunused /* nothing. */
#define pwh__forceinline /* nothing. */
#define pwh__unreachable() abort()
#define pwh__debugtrap() abort()
#define pwh__typeof(t) unsigned long
#endif

/* Convenience macros. */

#define donteliminate pwh__used
#define mustuse pwh__mustuse
#define threadlocal pwh__threadlocal
#define wontreturn pwh__wontreturn
#define maybeunused pwh__maybeunused
#define forceinline pwh__forceinline
#define fallthrough pwh__fallthrough

#if !defined likely
#define likely(x) pwh__likely(x)
#endif

#if !defined unlikely
#define unlikely(x) pwh__unlikely(x)
#endif

#define unused(x) ((void) (x))

#define maximum(i, j) (((i) > (j)) ? (i) : (j))
#define minimum(i, j) (((i) < (j)) ? (i) : (j))

#define countof(arr) (sizeof(arr) / sizeof(*(arr)))

#define memeq(a, b, size) (memcmp(a, b, size) == 0)
#define streq(a, b) (strcmp(a, b) == 0)

/* Helper functions. */

forceinline u64
pwh_hash_djb2(u64 seed, const u8 *data, usize data_size)
{
	u64 hash = seed;
	for (usize i = 0; i < data_size; i++)
	{
		hash = ((hash << 5) + hash) + data[i];
	}
	return hash;
}

#endif /* PWH_COMMON_H. */
