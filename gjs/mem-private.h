/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
// SPDX-License-Identifier: MIT OR LGPL-2.0-or-later
// SPDX-FileCopyrightText: 2008 litl, LLC
// SPDX-FileCopyrightText: 2021 Canonical, Ltd

#ifndef GJS_MEM_PRIVATE_H_
#define GJS_MEM_PRIVATE_H_

#include <atomic>

// clang-format off
#define GJS_FOR_EACH_COUNTER(macro) \
    macro(boxed_instance, 0)        \
    macro(boxed_prototype, 1)       \
    macro(closure, 2)               \
    macro(function, 3)              \
    macro(fundamental_instance, 4)  \
    macro(fundamental_prototype, 5) \
    macro(gerror_instance, 6)       \
    macro(gerror_prototype, 7)      \
    macro(interface, 8)             \
    macro(module, 9)                \
    macro(ns, 10)                   \
    macro(object_instance, 11)      \
    macro(object_prototype, 12)     \
    macro(param, 13)                \
    macro(union_instance, 14)       \
    macro(union_prototype, 15)
// clang-format on

namespace Gjs {
namespace Memory {

struct Counter {
    explicit Counter(const char* n) : name(n) {}
    std::atomic_int64_t value = ATOMIC_VAR_INIT(0);
    const char* name;
};

namespace Counters {
#define GJS_DECLARE_COUNTER(name, ix) extern Counter name;
GJS_DECLARE_COUNTER(everything, -1)
GJS_FOR_EACH_COUNTER(GJS_DECLARE_COUNTER)
#undef GJS_DECLARE_COUNTER

template <Counter* counter>
constexpr void inc() {
    everything.value++;
    counter->value++;
}

template <Counter* counter>
constexpr void dec() {
    counter->value--;
    everything.value--;
}

}  // namespace Counters
}  // namespace Memory
}  // namespace Gjs

#define GJS_INC_COUNTER(name) \
    (Gjs::Memory::Counters::inc<&Gjs::Memory::Counters::name>());

#define GJS_DEC_COUNTER(name) \
    (Gjs::Memory::Counters::dec<&Gjs::Memory::Counters::name>());

#define GJS_GET_COUNTER(name) (Gjs::Memory::Counters::name.value.load())

#endif  // GJS_MEM_PRIVATE_H_
