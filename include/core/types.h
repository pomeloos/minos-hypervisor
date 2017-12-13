#ifndef _TYPES_H
#define _TYPES_H

typedef long			_s64;
typedef unsigned long		_u64;

typedef unsigned int		_u32;
typedef unsigned short		_u16;
typedef unsigned char		_u8;
typedef int			_s32;
typedef short			_s16;
typedef char			_s8;

typedef _u32 	uint32_t;
typedef _s32 	int32_t;
typedef _u16 	uint16_t;
typedef _s16 	int16_t;
typedef _u8 	uint8_t;
typedef _s8 	int8_t;
typedef _u64 	uint64_t;
typedef _s64	int64_t;

typedef _u32			u32;
typedef _u16			u16;
typedef _u8			u8;
typedef _u64			u64;
typedef _s32			s32;
typedef _s16			s16;
typedef _s8			s8;
typedef _s64			s64;

typedef int			pid_t;
typedef int			size_t;

typedef int			irq_t;

typedef unsigned long		address_t;
typedef unsigned long 		stack_t;
typedef u32			dev_t;
typedef signed long		offset_t;
typedef long			time_t;
typedef u32			block_t;
typedef u16			gid_t;
typedef	u16			uid_t;
typedef u8			bool;

#define MAX(a, b)	a > b ? a : b
#define MIN(a, b)	a < b ? a : b

#define u8_to_u16(low, high)	((high << 8) | low)
#define u8_to_u32(u1, u2, u3, u4)	\
	((u4 << 24) | (u3 << 16) | (u2 << 8) | (u1))
#define u16_to_u32(low, high)	((high << 16) | low)

#define NULL ((void *)0)

#define container_of(ptr, name, member) \
	(name *)((unsigned char *)ptr - ((unsigned char *)&(((name *)0)->member)))

#define BIT(nr) (1 << (nr))

#define ALIGN(num, size)	((num) & ~(size - 1))

#define __user

#endif
