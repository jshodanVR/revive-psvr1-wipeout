// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>

#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/json/json.h"
#include "core/libraries/libs.h"

namespace Libraries::Json {

struct StringImpl {
    std::string text;
};

struct ArrayImpl {
    // A deque keeps its elements where they are when more are added: references to them are
    // handed out.
    std::deque<Value> items;
};

struct ObjectImpl {
    std::map<std::string, std::unique_ptr<Pair>> members;
};

struct IteratorImpl {
    ObjectImpl* object;
    std::map<std::string, std::unique_ptr<Pair>>::iterator at;
};

namespace {

void Clear(Value& value);
void Copy(Value& target, const Value& source);

void Init(Value& value) {
    std::memset(static_cast<void*>(&value), 0, sizeof(value));
}

void FreeObject(ObjectImpl* object) {
    if (object == nullptr) {
        return;
    }
    for (auto& [key, pair] : object->members) {
        delete pair->first.impl;
        Clear(pair->second);
    }
    delete object;
}

void FreeArray(ArrayImpl* array) {
    if (array == nullptr) {
        return;
    }
    for (Value& item : array->items) {
        Clear(item);
    }
    delete array;
}

void Clear(Value& value) {
    switch (value.type) {
    case Type::String:
        delete value.string;
        break;
    case Type::Array:
        FreeArray(value.array);
        break;
    case Type::Object:
        FreeObject(value.object);
        break;
    default:
        break;
    }
    Init(value);
}

Value& Member(ObjectImpl& object, const std::string& key) {
    auto& slot = object.members[key];
    if (!slot) {
        slot = std::make_unique<Pair>();
        slot->first.impl = new StringImpl{key};
        slot->unused = 0;
        Init(slot->second);
    }
    return slot->second;
}

ObjectImpl* CopyObject(const ObjectImpl* source) {
    auto* object = new ObjectImpl;
    if (source != nullptr) {
        for (const auto& [key, pair] : source->members) {
            Copy(Member(*object, key), pair->second);
        }
    }
    return object;
}

ArrayImpl* CopyArray(const ArrayImpl* source) {
    auto* array = new ArrayImpl;
    if (source != nullptr) {
        for (const Value& item : source->items) {
            Init(array->items.emplace_back());
            Copy(array->items.back(), item);
        }
    }
    return array;
}

/// `target` holds nothing that has to be freed.
void Copy(Value& target, const Value& source) {
    std::memcpy(static_cast<void*>(&target), &source, sizeof(Value));
    switch (source.type) {
    case Type::String:
        target.string = new StringImpl{source.string != nullptr ? source.string->text : ""};
        break;
    case Type::Array:
        target.array = CopyArray(source.array);
        break;
    case Type::Object:
        target.object = CopyObject(source.object);
        break;
    default:
        break;
    }
}

void SetNumber(Value& value, Type type, s64 integer, double real) {
    Init(value);
    value.type = type;
    value.integer = integer;
    value.real = real;
}

const Value& NullValue() {
    static const Value null{};
    return null;
}

// --- text to values ---------------------------------------------------------------------------

class Reader {
public:
    Reader(const char* text, u64 size) : at{text}, end{text + size} {}

    bool ReadDocument(Value& value) {
        if (!ReadValue(value, 0)) {
            return false;
        }
        SkipSpace();
        // Titles pass buffers with their terminator, or padded.
        while (at < end && *at == '\0') {
            ++at;
        }
        return at == end;
    }

private:
    static constexpr int MaxDepth = 64;

    void SkipSpace() {
        while (at < end && (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')) {
            ++at;
        }
    }

    bool Literal(const char* word) {
        const size_t length = std::strlen(word);
        if (static_cast<size_t>(end - at) < length || std::memcmp(at, word, length) != 0) {
            return false;
        }
        at += length;
        return true;
    }

    static void AppendUtf8(std::string& text, u32 code) {
        if (code < 0x80) {
            text += static_cast<char>(code);
        } else if (code < 0x800) {
            text += static_cast<char>(0xc0 | (code >> 6));
            text += static_cast<char>(0x80 | (code & 0x3f));
        } else if (code < 0x10000) {
            text += static_cast<char>(0xe0 | (code >> 12));
            text += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            text += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            text += static_cast<char>(0xf0 | (code >> 18));
            text += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            text += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            text += static_cast<char>(0x80 | (code & 0x3f));
        }
    }

    bool ReadHex4(u32& code) {
        if (end - at < 4) {
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *at++;
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code |= static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= static_cast<u32>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= static_cast<u32>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool ReadString(std::string& text) {
        if (at >= end || *at != '"') {
            return false;
        }
        ++at;
        while (at < end && *at != '"') {
            char c = *at++;
            if (c != '\\') {
                text += c;
                continue;
            }
            if (at >= end) {
                return false;
            }
            c = *at++;
            switch (c) {
            case 'n':
                text += '\n';
                break;
            case 't':
                text += '\t';
                break;
            case 'r':
                text += '\r';
                break;
            case 'b':
                text += '\b';
                break;
            case 'f':
                text += '\f';
                break;
            case 'u': {
                u32 code = 0;
                if (!ReadHex4(code)) {
                    return false;
                }
                // Two escapes in a row make up one character beyond the basic plane.
                if (code >= 0xd800 && code < 0xdc00 && end - at >= 6 && at[0] == '\\' &&
                    at[1] == 'u') {
                    at += 2;
                    u32 low = 0;
                    if (!ReadHex4(low)) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
                AppendUtf8(text, code);
                break;
            }
            default:
                // Quote, backslash, slash: themselves.
                text += c;
                break;
            }
        }
        if (at >= end) {
            return false;
        }
        ++at;
        return true;
    }

    bool ReadNumber(Value& value) {
        const char* begin = at;
        bool real = false;
        if (at < end && (*at == '-' || *at == '+')) {
            ++at;
        }
        while (at < end && ((*at >= '0' && *at <= '9') || *at == '.' || *at == 'e' || *at == 'E' ||
                            *at == '-' || *at == '+')) {
            real |= *at == '.' || *at == 'e' || *at == 'E';
            ++at;
        }
        const std::string text(begin, at);
        if (text.empty() || text == "-" || text == "+") {
            return false;
        }
        if (real) {
            const double number = std::strtod(text.c_str(), nullptr);
            SetNumber(value, Type::Real, static_cast<s64>(number), number);
        } else if (text[0] == '-') {
            const s64 number = std::strtoll(text.c_str(), nullptr, 10);
            SetNumber(value, Type::Integer, number, static_cast<double>(number));
        } else {
            const u64 number = std::strtoull(text.c_str(), nullptr, 10);
            SetNumber(value, Type::UInteger, static_cast<s64>(number), static_cast<double>(number));
        }
        return true;
    }

    bool ReadValue(Value& value, int depth) {
        if (depth > MaxDepth) {
            return false;
        }
        SkipSpace();
        if (at >= end) {
            return false;
        }
        switch (*at) {
        case '{': {
            ++at;
            Init(value);
            value.type = Type::Object;
            value.object = new ObjectImpl;
            SkipSpace();
            if (at < end && *at == '}') {
                ++at;
                return true;
            }
            while (true) {
                SkipSpace();
                std::string key;
                if (!ReadString(key)) {
                    return false;
                }
                SkipSpace();
                if (at >= end || *at != ':') {
                    return false;
                }
                ++at;
                Value& member = Member(*value.object, key);
                Clear(member);
                if (!ReadValue(member, depth + 1)) {
                    return false;
                }
                SkipSpace();
                if (at < end && *at == ',') {
                    ++at;
                    continue;
                }
                if (at < end && *at == '}') {
                    ++at;
                    return true;
                }
                return false;
            }
        }
        case '[': {
            ++at;
            Init(value);
            value.type = Type::Array;
            value.array = new ArrayImpl;
            SkipSpace();
            if (at < end && *at == ']') {
                ++at;
                return true;
            }
            while (true) {
                Init(value.array->items.emplace_back());
                if (!ReadValue(value.array->items.back(), depth + 1)) {
                    return false;
                }
                SkipSpace();
                if (at < end && *at == ',') {
                    ++at;
                    continue;
                }
                if (at < end && *at == ']') {
                    ++at;
                    return true;
                }
                return false;
            }
        }
        case '"': {
            std::string text;
            if (!ReadString(text)) {
                return false;
            }
            Init(value);
            value.type = Type::String;
            value.string = new StringImpl{std::move(text)};
            return true;
        }
        case 't':
            if (!Literal("true")) {
                return false;
            }
            Init(value);
            value.type = Type::Boolean;
            value.boolean = true;
            return true;
        case 'f':
            if (!Literal("false")) {
                return false;
            }
            Init(value);
            value.type = Type::Boolean;
            return true;
        case 'n':
            if (!Literal("null")) {
                return false;
            }
            Init(value);
            return true;
        default:
            return ReadNumber(value);
        }
    }

    const char* at;
    const char* end;
};

// --- values to text ---------------------------------------------------------------------------

void WriteString(std::string& out, const std::string& text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04x", c);
                out += escape;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void WriteNumber(std::string& out, const Value& value) {
    char text[40];
    switch (value.type) {
    case Type::Integer:
        std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value.integer));
        break;
    case Type::UInteger:
        std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value.uinteger));
        break;
    default:
        if (std::isfinite(value.real)) {
            std::snprintf(text, sizeof(text), "%.17g", value.real);
        } else {
            std::snprintf(text, sizeof(text), "null");
        }
        break;
    }
    out += text;
}

void Write(std::string& out, const Value& value) {
    switch (value.type) {
    case Type::Null:
        out += "null";
        break;
    case Type::Boolean:
        out += value.boolean ? "true" : "false";
        break;
    case Type::Integer:
    case Type::UInteger:
    case Type::Real:
        WriteNumber(out, value);
        break;
    case Type::String:
        WriteString(out, value.string != nullptr ? value.string->text : "");
        break;
    case Type::Array: {
        out += '[';
        bool first = true;
        if (value.array != nullptr) {
            for (const Value& item : value.array->items) {
                if (!first) {
                    out += ',';
                }
                first = false;
                Write(out, item);
            }
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        bool first = true;
        if (value.object != nullptr) {
            for (const auto& [key, pair] : value.object->members) {
                if (!first) {
                    out += ',';
                }
                first = false;
                WriteString(out, key);
                out += ':';
                Write(out, pair->second);
            }
        }
        out += '}';
        break;
    }
    }
}

void Assign(String* string, std::string text) {
    if (string->impl == nullptr) {
        string->impl = new StringImpl;
    }
    string->impl->text = std::move(text);
}

const std::string& TextOf(const String* string) {
    static const std::string empty;
    return string != nullptr && string->impl != nullptr ? string->impl->text : empty;
}

} // namespace

// --- sce::Json::Initializer, MemAllocator -------------------------------------------------------

// Both classes are empty as far as this implementation goes: memory comes from the emulator's
// heap rather than from the allocator the title hands over.
void PS4_SYSV_ABI InitializerConstruct(void*) {}
void PS4_SYSV_ABI InitializerDestruct(void*) {}
s32 PS4_SYSV_ABI InitializerInitialize(void*, const void*) {
    return 0;
}
s32 PS4_SYSV_ABI InitializerTerminate(void*) {
    return 0;
}
void PS4_SYSV_ABI MemAllocatorConstruct(void*) {}
void PS4_SYSV_ABI MemAllocatorDestruct(void*) {}

// --- sce::Json::String ----------------------------------------------------------------------------

void PS4_SYSV_ABI StringConstruct(String* self) {
    self->impl = new StringImpl;
}

void PS4_SYSV_ABI StringConstructFrom(String* self, const char* text) {
    self->impl = new StringImpl{text != nullptr ? text : ""};
}

void PS4_SYSV_ABI StringDestruct(String* self) {
    delete self->impl;
    self->impl = nullptr;
}

const char* PS4_SYSV_ABI StringCStr(const String* self) {
    return TextOf(self).c_str();
}

u64 PS4_SYSV_ABI StringSize(const String* self) {
    return TextOf(self).size();
}

// --- sce::Json::Value -----------------------------------------------------------------------------

void PS4_SYSV_ABI ValueConstruct(Value* self) {
    Init(*self);
}

void PS4_SYSV_ABI ValueConstructBool(Value* self, bool boolean) {
    Init(*self);
    self->type = Type::Boolean;
    self->boolean = boolean;
}

void PS4_SYSV_ABI ValueConstructInteger(Value* self, s64 integer) {
    SetNumber(*self, Type::Integer, integer, static_cast<double>(integer));
}

void PS4_SYSV_ABI ValueConstructUInteger(Value* self, u64 integer) {
    Init(*self);
    self->type = Type::UInteger;
    self->uinteger = integer;
    self->real = static_cast<double>(integer);
}

void PS4_SYSV_ABI ValueClear(Value* self) {
    Clear(*self);
}

void PS4_SYSV_ABI ValueConstructReal(Value* self, double real) {
    SetNumber(*self, Type::Real, static_cast<s64>(real), real);
}

void PS4_SYSV_ABI ValueConstructString(Value* self, const String* string) {
    Init(*self);
    self->type = Type::String;
    self->string = new StringImpl{TextOf(string)};
}

void PS4_SYSV_ABI ValueConstructArray(Value* self, const Array* array) {
    Init(*self);
    self->type = Type::Array;
    self->array = CopyArray(array != nullptr ? array->impl : nullptr);
}

void PS4_SYSV_ABI ValueConstructObject(Value* self, const Object* object) {
    Init(*self);
    self->type = Type::Object;
    self->object = CopyObject(object != nullptr ? object->impl : nullptr);
}

void PS4_SYSV_ABI ValueConstructCopy(Value* self, const Value* other) {
    Init(*self);
    if (other != nullptr) {
        Copy(*self, *other);
    }
}

void PS4_SYSV_ABI ValueDestruct(Value* self) {
    Clear(*self);
}

Value* PS4_SYSV_ABI ValueAssign(Value* self, const Value* other) {
    if (self != other && other != nullptr) {
        // The source may be part of what is being replaced.
        Value copy;
        Init(copy);
        Copy(copy, *other);
        Clear(*self);
        std::memcpy(static_cast<void*>(self), &copy, sizeof(Value));
    }
    return self;
}

Value* PS4_SYSV_ABI ValueSetObject(Value* self, const Object* object) {
    ObjectImpl* copy = CopyObject(object != nullptr ? object->impl : nullptr);
    Clear(*self);
    self->type = Type::Object;
    self->object = copy;
    return self;
}

s32 PS4_SYSV_ABI ValueGetType(const Value* self) {
    return static_cast<s32>(self->type);
}

const bool* PS4_SYSV_ABI ValueGetBoolean(const Value* self) {
    return &self->boolean;
}

const s64* PS4_SYSV_ABI ValueGetInteger(const Value* self) {
    return &self->integer;
}

const u64* PS4_SYSV_ABI ValueGetUInteger(const Value* self) {
    return &self->uinteger;
}

const double* PS4_SYSV_ABI ValueGetReal(const Value* self) {
    return &self->real;
}

const Object* PS4_SYSV_ABI ValueGetObject(const Value* self) {
    // An Object is a pointer to its contents, and that is what an object value holds.
    static ObjectImpl empty_contents;
    static const Object empty{&empty_contents};
    if (self->type != Type::Object || self->object == nullptr) {
        return &empty;
    }
    return reinterpret_cast<const Object*>(&self->object);
}

bool PS4_SYSV_ABI ValueToBool(const Value* self) {
    return self->type != Type::Null;
}

s32 PS4_SYSV_ABI ValueCount(const Value* self) {
    if (self->type == Type::Array && self->array != nullptr) {
        return static_cast<s32>(self->array->items.size());
    }
    if (self->type == Type::Object && self->object != nullptr) {
        return static_cast<s32>(self->object->members.size());
    }
    return 0;
}

const Value* PS4_SYSV_ABI ValueIndex(const Value* self, u64 index) {
    if (self->type == Type::Array && self->array != nullptr && index < self->array->items.size()) {
        return &self->array->items[index];
    }
    return &NullValue();
}

const Value* PS4_SYSV_ABI ValueMember(const Value* self, const char* key) {
    if (self->type == Type::Object && self->object != nullptr && key != nullptr) {
        const auto it = self->object->members.find(key);
        if (it != self->object->members.end()) {
            return &it->second->second;
        }
    }
    return &NullValue();
}

Value* PS4_SYSV_ABI ValueReferValue(Value* self, const String* key) {
    if (self->type == Type::Object && self->object != nullptr) {
        const auto it = self->object->members.find(TextOf(key));
        if (it != self->object->members.end()) {
            return &it->second->second;
        }
    }
    return nullptr;
}

void PS4_SYSV_ABI ValueToString(const Value* self, String* out) {
    if (out == nullptr) {
        return;
    }
    if (self->type == Type::String) {
        Assign(out, self->string != nullptr ? self->string->text : "");
        return;
    }
    std::string text;
    Write(text, *self);
    Assign(out, std::move(text));
}

s32 PS4_SYSV_ABI ValueSerialize(Value* self, String* out) {
    if (out != nullptr) {
        std::string text;
        Write(text, *self);
        Assign(out, std::move(text));
    }
    return 0;
}

// --- sce::Json::Array -----------------------------------------------------------------------------

void PS4_SYSV_ABI ArrayConstruct(Array* self) {
    self->impl = new ArrayImpl;
}

void PS4_SYSV_ABI ArrayDestruct(Array* self) {
    FreeArray(self->impl);
    self->impl = nullptr;
}

void PS4_SYSV_ABI ArrayPushBack(Array* self, const Value* value) {
    if (self->impl == nullptr) {
        self->impl = new ArrayImpl;
    }
    Init(self->impl->items.emplace_back());
    if (value != nullptr) {
        Copy(self->impl->items.back(), *value);
    }
}

// --- sce::Json::Object ----------------------------------------------------------------------------

void PS4_SYSV_ABI ObjectConstruct(Object* self) {
    self->impl = new ObjectImpl;
}

void PS4_SYSV_ABI ObjectDestruct(Object* self) {
    FreeObject(self->impl);
    self->impl = nullptr;
}

Value* PS4_SYSV_ABI ObjectIndex(Object* self, const String* key) {
    if (self->impl == nullptr) {
        self->impl = new ObjectImpl;
    }
    return &Member(*self->impl, TextOf(key));
}

Iterator* PS4_SYSV_ABI ObjectBegin(Iterator* result, const Object* self) {
    result->impl = new IteratorImpl{self->impl, {}};
    if (self->impl != nullptr) {
        result->impl->at = self->impl->members.begin();
    }
    return result;
}

Iterator* PS4_SYSV_ABI ObjectEnd(Iterator* result, const Object* self) {
    result->impl = new IteratorImpl{self->impl, {}};
    if (self->impl != nullptr) {
        result->impl->at = self->impl->members.end();
    }
    return result;
}

void PS4_SYSV_ABI IteratorDestruct(Iterator* self) {
    delete self->impl;
    self->impl = nullptr;
}

Iterator* PS4_SYSV_ABI IteratorIncrement(Iterator* self) {
    if (self->impl != nullptr && self->impl->object != nullptr &&
        self->impl->at != self->impl->object->members.end()) {
        ++self->impl->at;
    }
    return self;
}

Pair* PS4_SYSV_ABI IteratorDereference(const Iterator* self) {
    static Pair nothing{};
    if (self->impl == nullptr || self->impl->object == nullptr ||
        self->impl->at == self->impl->object->members.end()) {
        return &nothing;
    }
    return self->impl->at->second.get();
}

bool PS4_SYSV_ABI IteratorNotEqual(const Iterator* self, const Iterator* other) {
    const IteratorImpl* a = self->impl;
    const IteratorImpl* b = other->impl;
    if (a == nullptr || b == nullptr || a->object == nullptr || b->object == nullptr) {
        return false;
    }
    return a->object != b->object || a->at != b->at;
}

// --- sce::Json::Parser ----------------------------------------------------------------------------

s32 PS4_SYSV_ABI ParserParse(Value* value, const char* text, u64 size) {
    static int trace = [] {
        const char* setting = std::getenv("SHADPS4_JSON_TRACE");
        return setting != nullptr ? std::atoi(setting) : 0;
    }();
    if (trace > 0 && text != nullptr) {
        --trace;
        std::string shown(text, std::min<u64>(size, 600));
        for (char& c : shown) {
            if (c == '\n' || c == '\r' || c == '\t') {
                c = ' ';
            }
        }
        LOG_INFO(Lib_SysModule, "JSON to parse ({} bytes): {}", size, shown);
    }

    Value parsed;
    Init(parsed);
    if (text == nullptr || !Reader{text, size}.ReadDocument(parsed)) {
        // Not JSON (titles hand over whatever a field holds, "None" for one): the value stays
        // what it was, which is null for one freshly made.
        Clear(parsed);
        return static_cast<s32>(0x80920101);
    }
    Clear(*value);
    std::memcpy(static_cast<void*>(value), &parsed, sizeof(Value));
    return 0;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    // SHADPS4_JSON=0 leaves the library to the firmware module, or to stubs.
    if (const char* setting = std::getenv("SHADPS4_JSON"); setting != nullptr && setting[0] == '0') {
        return;
    }
#define add(nid, function) LIB_FUNCTION(nid, "libSceJson2", 1, "libSceJson", function)
    add("cK6bYHf-Q5E", InitializerConstruct);
    add("RujUxbr3haM", InitializerDestruct);
    add("Cxwy7wHq4J0", InitializerInitialize);
    add("PR5k1penBLM", InitializerTerminate);
    add("-hJRce8wn1U", MemAllocatorConstruct);
    add("OcAgPxcq5Vk", MemAllocatorDestruct);

    add("qSmqLXXCPas", StringConstruct);
    add("9KUZFjI1IxA", StringConstructFrom);
    add("cG1VE2HMl6c", StringDestruct);
    add("L1KAkYWml-M", StringCStr);
    add("UVBkipSD2sw", StringSize);

    add("qBMjqyBn3OM", ValueConstruct);
    add("UeuWT+yNdCQ", ValueConstructBool);
    add("0lLK8+kDqmE", ValueConstructInteger);
    add("x4AUdbhpRB0", ValueConstructUInteger);
    add("FIjXN2TkuTs", ValueClear);
    add("sOmU4vnx3s0", ValueConstructReal);
    add("sZIoMRGO+jk", ValueConstructString);
    add("iZeYfOxtMRg", ValueConstructArray);
    add("3xUXnmUkXfo", ValueConstructObject);
    add("fSb2oQTNrgA", ValueConstructCopy);
    add("WTtYf+cNnXI", ValueDestruct);
    add("4zrm6VrgIAw", ValueAssign);
    add("dFCphqnd+a4", ValueSetObject);
    add("SHtAad20YYM", ValueGetType);
    add("zTwZdI8AZ5Y", ValueGetBoolean);
    add("DIxvoy7Ngvk", ValueGetInteger);
    add("sn4HNCtNRzY", ValueGetUInteger);
    add("3qrge7L-AU4", ValueGetReal);
    add("IlsmvBtMkak", ValueGetObject);
    add("a-aMMUXqrN0", ValueToBool);
    add("RBw+4NukeGQ", ValueCount);
    add("XlWbvieLj2M", ValueIndex);
    add("HwDt5lD9Bfo", ValueMember);
    add("wLsJlmgEIaI", ValueReferValue);
    add("Ncel8t2Rrpc", ValueToString);
    add("R7FDWtcN6f8", ValueSerialize);

    add("JP-PtKMiI1E", ArrayConstruct);
    add("HJ8GpRT1aiw", ArrayDestruct);
    add("zQtLRTqceMY", ArrayPushBack);

    add("OJPTonqdg0I", ObjectConstruct);
    add("5JmzZt8twAo", ObjectDestruct);
    add("ERuf9y0DY84", ObjectIndex);
    add("xhAcaIwnrgk", ObjectBegin);
    add("ivMCitpSQNk", ObjectEnd);
    add("hoINmSMlYjI", IteratorDestruct);
    add("DlWmn2ZQuWY", IteratorIncrement);
    add("ZCd6IYoD3Bc", IteratorDereference);
    add("+isUKw4zud4", IteratorNotEqual);

    add("S5JxQnoGF3E", ParserParse);
#undef add
}

} // namespace Libraries::Json
