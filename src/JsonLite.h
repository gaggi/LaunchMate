#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace jsonlite
{
    struct Value;
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;

    struct Value
    {
        using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
        Storage data;

        Value() : data(nullptr) {}
        Value(bool v) : data(v) {}
        Value(double v) : data(v) {}
        Value(std::string v) : data(std::move(v)) {}
        Value(Array v) : data(std::move(v)) {}
        Value(Object v) : data(std::move(v)) {}

        bool IsObject() const;
        bool IsArray() const;
        bool IsString() const;
        bool IsBool() const;
        bool IsNumber() const;

        const Object& AsObject() const;
        const Array& AsArray() const;
        const std::string& AsString() const;
        bool AsBool(bool fallback = false) const;
        double AsNumber(double fallback = 0.0) const;
    };

    Value Parse(const std::string& input);
    std::string Serialize(const Value& value, int indentSize = 2);
}
