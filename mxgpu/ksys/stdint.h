/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
#ifndef MXGPU_KSYS_STDINT_H
#define MXGPU_KSYS_STDINT_H
#include <linux/types.h>
#ifndef MXGPU_UINT_DEFINED
#define MXGPU_UINT_DEFINED
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
typedef s8 int8_t;
typedef s16 int16_t;
typedef s32 int32_t;
typedef s64 int64_t;
typedef unsigned long uintptr_t;
#define UINT32_MAX ((uint32_t)~0u)
#define UINT64_MAX ((uint64_t)~0ull)
#endif
#endif
