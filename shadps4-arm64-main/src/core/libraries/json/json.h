// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

// libSceJson2 (sce::Json), the firmware's JSON library. Titles only ever go through its exported
// member functions, so the objects they hold are opaque to them and laid out here as suits this
// implementation, within the sizes the real classes have: a Value is 32 bytes (titles keep them
// in vectors), a String and an Object::iterator one pointer, and what an Object iterator points
// at is a String followed, 16 bytes in, by a Value.

namespace Libraries::Json {

enum class Type : s32 {
    Null = 0,
    Boolean = 1,
    Integer = 2,
    UInteger = 3,
    Real = 4,
    String = 5,
    Array = 6,
    Object = 7,
};

struct StringImpl;
struct ArrayImpl;
struct ObjectImpl;
struct IteratorImpl;

/// sce::Json::String
struct String {
    StringImpl* impl;
};

/// sce::Json::Value. Numbers are kept both as an integer and as a real, so that the reference
/// either getter returns has the number in it whichever of the two the text was.
struct alignas(16) Value {
    union {
        bool boolean;
        s64 integer;
        u64 uinteger;
    };
    union {
        double real;
        StringImpl* string;
        ArrayImpl* array;
        ObjectImpl* object;
    };
    Type type;
    u32 unused;
    u64 reserved;
};
static_assert(sizeof(Value) == 32);

/// What dereferencing an Object::iterator gives.
struct Pair {
    String first;
    u64 unused;
    Value second;
};
static_assert(offsetof(Pair, second) == 16);

/// sce::Json::Array and sce::Json::Object
struct Array {
    ArrayImpl* impl;
};
struct Object {
    ObjectImpl* impl;
};

/// sce::Json::Object::iterator
struct Iterator {
    IteratorImpl* impl;
};

void PS4_SYSV_ABI StringConstruct(String* self);
void PS4_SYSV_ABI StringConstructFrom(String* self, const char* text);
void PS4_SYSV_ABI StringDestruct(String* self);
const char* PS4_SYSV_ABI StringCStr(const String* self);
u64 PS4_SYSV_ABI StringSize(const String* self);

void PS4_SYSV_ABI ValueConstruct(Value* self);
void PS4_SYSV_ABI ValueConstructBool(Value* self, bool boolean);
void PS4_SYSV_ABI ValueConstructInteger(Value* self, s64 integer);
void PS4_SYSV_ABI ValueConstructUInteger(Value* self, u64 integer);
void PS4_SYSV_ABI ValueClear(Value* self);
void PS4_SYSV_ABI ValueConstructReal(Value* self, double real);
void PS4_SYSV_ABI ValueConstructString(Value* self, const String* string);
void PS4_SYSV_ABI ValueConstructArray(Value* self, const Array* array);
void PS4_SYSV_ABI ValueConstructObject(Value* self, const Object* object);
void PS4_SYSV_ABI ValueConstructCopy(Value* self, const Value* other);
void PS4_SYSV_ABI ValueDestruct(Value* self);
Value* PS4_SYSV_ABI ValueAssign(Value* self, const Value* other);
Value* PS4_SYSV_ABI ValueSetObject(Value* self, const Object* object);
s32 PS4_SYSV_ABI ValueGetType(const Value* self);
const bool* PS4_SYSV_ABI ValueGetBoolean(const Value* self);
const s64* PS4_SYSV_ABI ValueGetInteger(const Value* self);
const u64* PS4_SYSV_ABI ValueGetUInteger(const Value* self);
const double* PS4_SYSV_ABI ValueGetReal(const Value* self);
const Object* PS4_SYSV_ABI ValueGetObject(const Value* self);
bool PS4_SYSV_ABI ValueToBool(const Value* self);
s32 PS4_SYSV_ABI ValueCount(const Value* self);
const Value* PS4_SYSV_ABI ValueIndex(const Value* self, u64 index);
const Value* PS4_SYSV_ABI ValueMember(const Value* self, const char* key);
Value* PS4_SYSV_ABI ValueReferValue(Value* self, const String* key);
void PS4_SYSV_ABI ValueToString(const Value* self, String* out);
s32 PS4_SYSV_ABI ValueSerialize(Value* self, String* out);

void PS4_SYSV_ABI ArrayConstruct(Array* self);
void PS4_SYSV_ABI ArrayDestruct(Array* self);
void PS4_SYSV_ABI ArrayPushBack(Array* self, const Value* value);

void PS4_SYSV_ABI ObjectConstruct(Object* self);
void PS4_SYSV_ABI ObjectDestruct(Object* self);
Value* PS4_SYSV_ABI ObjectIndex(Object* self, const String* key);
Iterator* PS4_SYSV_ABI ObjectBegin(Iterator* result, const Object* self);
Iterator* PS4_SYSV_ABI ObjectEnd(Iterator* result, const Object* self);
void PS4_SYSV_ABI IteratorDestruct(Iterator* self);
Iterator* PS4_SYSV_ABI IteratorIncrement(Iterator* self);
Pair* PS4_SYSV_ABI IteratorDereference(const Iterator* self);
bool PS4_SYSV_ABI IteratorNotEqual(const Iterator* self, const Iterator* other);

/// 0 when `text` is a JSON document; `value` then holds it and is left alone otherwise.
s32 PS4_SYSV_ABI ParserParse(Value* value, const char* text, u64 size);

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Json
