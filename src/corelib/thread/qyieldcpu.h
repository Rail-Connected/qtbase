// Copyright (C) 2023 The Qt Company Ltd.
// Copyright (C) 2023 Intel Corporation.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QYIELDCPU_H
#define QYIELDCPU_H

#include <QtCore/qcompilerdetection.h>
#include <QtCore/qprocessordetection.h>
#include <QtCore/qtconfigmacros.h>

#ifdef Q_CC_MSVC_ONLY
// MSVC defines _YIELD_PROCESSOR() in <xatomic.h>, but as that is a private
// header, we include the public ones
#  ifdef __cplusplus
#    include <atomic>
extern "C"
#  endif
void _mm_pause(void);       // the compiler recognizes as intrinsic
#endif

QT_BEGIN_NAMESPACE

#ifdef Q_CC_GNU
__attribute__((artificial))
#endif
Q_ALWAYS_INLINE void qYieldCpu(void) Q_DECL_NOEXCEPT;

void qYieldCpu(void)
#ifdef __cplusplus
    noexcept
#endif
{
#if __has_builtin(__yield)
    __yield();              // Generic
#elif defined(_YIELD_PROCESSOR) && defined(Q_CC_MSVC)
    _YIELD_PROCESSOR();     // Generic; MSVC's <atomic>

#elif __has_builtin(__builtin_ia32_pause)
    __builtin_ia32_pause();
#elif defined(Q_PROCESSOR_X86) && defined(Q_CC_GNU)
    // GCC < 10 didn't have __has_builtin()
    __builtin_ia32_pause();
#elif defined(Q_PROCESSOR_X86) && defined(Q_CC_MSVC)
    _mm_pause();
#elif defined(Q_PROCESSOR_X86)
    asm("pause");           // hopefully asm() works in this compiler

#elif __has_builtin(__builtin_arm_yield)
    __builtin_arm_yield();
#elif defined(Q_PROCESSOR_ARM) && Q_PROCESSOR_ARM >= 7 && defined(Q_CC_GNU)
    asm("yield");           // this works everywhere

#elif __has_builtin(__builtin_riscv_pause)
    __builtin_riscv_pause();        // Zihintpause extension
#elif defined(Q_PROCESSOR_RISCV)
    asm("fence w, 0");              // a.k.a. "pause"

#elif defined(_YIELD_PROCESSOR) && defined(Q_CC_GHS)
    _YIELD_PROCESSOR;       // Green Hills (INTEGRITY), but only on ARM
#endif
}

QT_END_NAMESPACE

#endif // QYIELDCPU_H
