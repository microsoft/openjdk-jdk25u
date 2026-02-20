/*
 * Copyright (c) 2020, Microsoft Corporation. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef OS_CPU_WINDOWS_AARCH64_ATOMIC_WINDOWS_AARCH64_HPP
#define OS_CPU_WINDOWS_AARCH64_ATOMIC_WINDOWS_AARCH64_HPP

#include <intrin.h>
#include "runtime/os.hpp"
#include "runtime/vm_version.hpp"


// As per atomic.hpp all read-modify-write operations have to provide two-way
// barriers semantics. The memory_order parameter is ignored - we always provide
// the strongest/most-conservative ordering
//
// For AARCH64 we add explicit barriers in the stubs.

template<size_t byte_size>
struct Atomic::PlatformAdd {
  template<typename D, typename I>
  D add_then_fetch(D volatile* dest, I add_value, atomic_memory_order order) const;

  template<typename D, typename I>
  D fetch_then_add(D volatile* dest, I add_value, atomic_memory_order order) const {
    return add_then_fetch(dest, add_value, order) - add_value;
  }
};

// The Interlocked* APIs only take long and will not accept __int32. That is
// acceptable on Windows, since long is a 32-bits integer type.

#define DEFINE_INTRINSIC_ADD(IntrinsicName, IntrinsicType)                \
  template<>                                                              \
  template<typename D, typename I>                                        \
  inline D Atomic::PlatformAdd<sizeof(IntrinsicType)>::add_then_fetch(D volatile* dest, \
                                                                      I add_value, \
                                                                      atomic_memory_order order) const { \
    STATIC_ASSERT(sizeof(IntrinsicType) == sizeof(D));                    \
    return PrimitiveConversions::cast<D>(                                 \
      IntrinsicName(reinterpret_cast<IntrinsicType volatile *>(dest),     \
                    PrimitiveConversions::cast<IntrinsicType>(add_value))); \
  }

DEFINE_INTRINSIC_ADD(InterlockedAdd,   long)
DEFINE_INTRINSIC_ADD(InterlockedAdd64, __int64)

#undef DEFINE_INTRINSIC_ADD

#define DEFINE_INTRINSIC_XCHG(IntrinsicName, IntrinsicType)               \
  template<>                                                              \
  template<typename T>                                                    \
  inline T Atomic::PlatformXchg<sizeof(IntrinsicType)>::operator()(T volatile* dest, \
                                                                   T exchange_value, \
                                                                   atomic_memory_order order) const { \
    STATIC_ASSERT(sizeof(IntrinsicType) == sizeof(T));                    \
    return PrimitiveConversions::cast<T>(                                 \
      IntrinsicName(reinterpret_cast<IntrinsicType volatile *>(dest),     \
                    PrimitiveConversions::cast<IntrinsicType>(exchange_value))); \
  }

DEFINE_INTRINSIC_XCHG(InterlockedExchange,   long)
DEFINE_INTRINSIC_XCHG(InterlockedExchange64, __int64)

#undef DEFINE_INTRINSIC_XCHG

// Note: the order of the parameters is different between
// Atomic::PlatformCmpxchg<*>::operator() and the
// InterlockedCompareExchange* API.

#define DEFINE_INTRINSIC_CMPXCHG(IntrinsicName, IntrinsicType)            \
  template<>                                                              \
  template<typename T>                                                    \
  inline T Atomic::PlatformCmpxchg<sizeof(IntrinsicType)>::operator()(T volatile* dest, \
                                                                      T compare_value, \
                                                                      T exchange_value, \
                                                                      atomic_memory_order order) const { \
    STATIC_ASSERT(sizeof(IntrinsicType) == sizeof(T));                    \
    return PrimitiveConversions::cast<T>(                                 \
      IntrinsicName(reinterpret_cast<IntrinsicType volatile *>(dest),     \
                    PrimitiveConversions::cast<IntrinsicType>(exchange_value), \
                    PrimitiveConversions::cast<IntrinsicType>(compare_value))); \
  }

DEFINE_INTRINSIC_CMPXCHG(_InterlockedCompareExchange8, char) // Use the intrinsic as InterlockedCompareExchange8 does not exist
DEFINE_INTRINSIC_CMPXCHG(InterlockedCompareExchange,   long)
DEFINE_INTRINSIC_CMPXCHG(InterlockedCompareExchange64, __int64)

#undef DEFINE_INTRINSIC_CMPXCHG

// Override PlatformLoad and PlatformStore to use LDAR/STLR on Windows AArch64.
//
// Under MSVC /volatile:iso (the default for ARM64), the generic PlatformLoad
// and PlatformStore use plain volatile dereferences which generate plain LDR/STR
// instructions with NO acquire/release barriers. This is insufficient for ARM64's
// weak memory model in HotSpot's concurrent runtime code (ObjectMonitor Dekker
// protocols, ParkEvent signaling, etc.) where Atomic::load()/Atomic::store() are
// used in cross-thread communication patterns that depend on load/store ordering.
//
// On x86, this works by accident because x86-TSO provides store-release and
// load-acquire semantics for all memory accesses. On ARM64, we must explicitly
// use LDAR (acquire load) and STLR (release store) to get equivalent ordering.
//
// This matches the effective behavior of /volatile:ms but scoped only to
// Atomic:: operations rather than ALL volatile accesses, and ensures correct
// cross-core visibility for HotSpot's lock-free algorithms.

template<>
struct Atomic::PlatformLoad<1> {
  template<typename T>
  T operator()(T const volatile* dest) const {
    STATIC_ASSERT(sizeof(T) == 1);
    return PrimitiveConversions::cast<T>(
      __ldar8(reinterpret_cast<unsigned __int8 volatile*>(
        const_cast<T volatile*>(dest))));
  }
};

template<>
struct Atomic::PlatformLoad<2> {
  template<typename T>
  T operator()(T const volatile* dest) const {
    STATIC_ASSERT(sizeof(T) == 2);
    return PrimitiveConversions::cast<T>(
      __ldar16(reinterpret_cast<unsigned __int16 volatile*>(
        const_cast<T volatile*>(dest))));
  }
};

template<>
struct Atomic::PlatformLoad<4> {
  template<typename T>
  T operator()(T const volatile* dest) const {
    STATIC_ASSERT(sizeof(T) == 4);
    return PrimitiveConversions::cast<T>(
      __ldar32(reinterpret_cast<unsigned __int32 volatile*>(
        const_cast<T volatile*>(dest))));
  }
};

template<>
struct Atomic::PlatformLoad<8> {
  template<typename T>
  T operator()(T const volatile* dest) const {
    STATIC_ASSERT(sizeof(T) == 8);
    return PrimitiveConversions::cast<T>(
      __ldar64(reinterpret_cast<unsigned __int64 volatile*>(
        const_cast<T volatile*>(dest))));
  }
};

template<>
struct Atomic::PlatformStore<1> {
  template<typename T>
  void operator()(T volatile* dest, T new_value) const {
    STATIC_ASSERT(sizeof(T) == 1);
    __stlr8(reinterpret_cast<unsigned __int8 volatile*>(dest),
            PrimitiveConversions::cast<unsigned __int8>(new_value));
  }
};

template<>
struct Atomic::PlatformStore<2> {
  template<typename T>
  void operator()(T volatile* dest, T new_value) const {
    STATIC_ASSERT(sizeof(T) == 2);
    __stlr16(reinterpret_cast<unsigned __int16 volatile*>(dest),
             PrimitiveConversions::cast<unsigned __int16>(new_value));
  }
};

template<>
struct Atomic::PlatformStore<4> {
  template<typename T>
  void operator()(T volatile* dest, T new_value) const {
    STATIC_ASSERT(sizeof(T) == 4);
    __stlr32(reinterpret_cast<unsigned __int32 volatile*>(dest),
             PrimitiveConversions::cast<unsigned __int32>(new_value));
  }
};

template<>
struct Atomic::PlatformStore<8> {
  template<typename T>
  void operator()(T volatile* dest, T new_value) const {
    STATIC_ASSERT(sizeof(T) == 8);
    __stlr64(reinterpret_cast<unsigned __int64 volatile*>(dest),
             PrimitiveConversions::cast<unsigned __int64>(new_value));
  }
};

// Specialize PlatformOrderedLoad and PlatformOrderedStore to use MSVC's
// __ldar/__stlr intrinsics, matching the Linux AArch64 implementation which
// uses __atomic_load/__atomic_store with __ATOMIC_ACQUIRE/__ATOMIC_RELEASE.
// These emit single LDAR/STLR instructions that have acquire/release semantics
// baked in, rather than the generic fallback of separate dmb + plain load/store.
// On AArch64, LDAR/STLR provide stronger ordering guarantees than dmb + ldr/str
// for cross-core visibility (Dekker patterns, etc.).

template<>
struct Atomic::PlatformOrderedLoad<1, X_ACQUIRE> {
  template <typename T>
  T operator()(const volatile T* p) const {
    STATIC_ASSERT(sizeof(T) == 1);
    return PrimitiveConversions::cast<T>(
      __ldar8(reinterpret_cast<unsigned __int8 volatile*>(
        const_cast<T volatile*>(p))));
  }
};

template<>
struct Atomic::PlatformOrderedLoad<2, X_ACQUIRE> {
  template <typename T>
  T operator()(const volatile T* p) const {
    STATIC_ASSERT(sizeof(T) == 2);
    return PrimitiveConversions::cast<T>(
      __ldar16(reinterpret_cast<unsigned __int16 volatile*>(
        const_cast<T volatile*>(p))));
  }
};

template<>
struct Atomic::PlatformOrderedLoad<4, X_ACQUIRE> {
  template <typename T>
  T operator()(const volatile T* p) const {
    STATIC_ASSERT(sizeof(T) == 4);
    return PrimitiveConversions::cast<T>(
      __ldar32(reinterpret_cast<unsigned __int32 volatile*>(
        const_cast<T volatile*>(p))));
  }
};

template<>
struct Atomic::PlatformOrderedLoad<8, X_ACQUIRE> {
  template <typename T>
  T operator()(const volatile T* p) const {
    STATIC_ASSERT(sizeof(T) == 8);
    return PrimitiveConversions::cast<T>(
      __ldar64(reinterpret_cast<unsigned __int64 volatile*>(
        const_cast<T volatile*>(p))));
  }
};

template<>
struct Atomic::PlatformOrderedStore<1, RELEASE_X> {
  template <typename T>
  void operator()(volatile T* p, T v) const {
    STATIC_ASSERT(sizeof(T) == 1);
    __stlr8(reinterpret_cast<unsigned __int8 volatile*>(p),
            PrimitiveConversions::cast<unsigned __int8>(v));
  }
};

template<>
struct Atomic::PlatformOrderedStore<2, RELEASE_X> {
  template <typename T>
  void operator()(volatile T* p, T v) const {
    STATIC_ASSERT(sizeof(T) == 2);
    __stlr16(reinterpret_cast<unsigned __int16 volatile*>(p),
             PrimitiveConversions::cast<unsigned __int16>(v));
  }
};

template<>
struct Atomic::PlatformOrderedStore<4, RELEASE_X> {
  template <typename T>
  void operator()(volatile T* p, T v) const {
    STATIC_ASSERT(sizeof(T) == 4);
    __stlr32(reinterpret_cast<unsigned __int32 volatile*>(p),
             PrimitiveConversions::cast<unsigned __int32>(v));
  }
};

template<>
struct Atomic::PlatformOrderedStore<8, RELEASE_X> {
  template <typename T>
  void operator()(volatile T* p, T v) const {
    STATIC_ASSERT(sizeof(T) == 8);
    __stlr64(reinterpret_cast<unsigned __int64 volatile*>(p),
             PrimitiveConversions::cast<unsigned __int64>(v));
  }
};

// release_store + fence combination, matching Linux AArch64
template<size_t byte_size>
struct Atomic::PlatformOrderedStore<byte_size, RELEASE_X_FENCE> {
  template <typename T>
  void operator()(volatile T* p, T v) const {
    Atomic::release_store(p, v);
    OrderAccess::fence();
  }
};

#endif // OS_CPU_WINDOWS_AARCH64_ATOMIC_WINDOWS_AARCH64_HPP
