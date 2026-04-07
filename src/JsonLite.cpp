#include "JsonLite.h"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace jsonlite
{
    bool Value::IsObject() const { return std::holds_alternative<Object>(data); }
    bool Value::IsArray() const { return std::holds_alternative<Array>(data); }
    bool Value::IsString() const { return std::holds_alternative<std::string>(data); }
    bool Value::IsBool() const { return std::holds_alternative<bool>(data); }
    bool Value::IsNumber() const { return std::holds_alternative<double>(data); }
    const Object& Value::AsObject() const { return std::get<Object>(data); }
    const Array& Value::AsArray() const { return std::get<Array>(data); }
    const std::string& Value::AsString() const { return std::get<std::string>(data); }
    bool Value::AsBool(bool fallback) const { return IsBool() ? std::get<bool>(data) : fallback; }
    double Value::AsNumber(double fallback) const { return IsNumber() ? std::get<double>(data) : fallback; }

    namespace
    {
        class Parser
        {
        public:
            explicit Parser(const std::string& input) : input_(input) {}

            Value ParseValue()
            {
                SkipWhitespace();
                if (index_ >= input_.size())
                {
                    throw std::runtime_error("Unexpected end of JSON.");
                }

                switch (input_[index_])
                {
                case '{': return ParseObject();
                case '[': return ParseArray();
                case '"': return Value(ParseString());
                case 't': ConsumeLiteral("true"); return Value(true);
                case 'f': ConsumeLiteral("false"); return Value(false);
                case 'n': ConsumeLiteral("null"); return Value();
                default: return Value(ParseNumber());
                }
            }

        private:
            Value ParseObject()
            {
                Expect('{');
                Object object;
                SkipWhitespace();
                if (TryConsume('}'))
                {
                    return object;
                }

                while (true)
                {
                    SkipWhitespace();
                    const auto key = ParseString();
                    SkipWhitespace();
                    Expect(':');
                    object.emplace(key, ParseValue());
                    SkipWhitespace();
                    if (TryConsume('}'))
                    {
                        break;
                    }

                    Expect(',');
                }

                return object;
            }

            Value ParseArray()
            {
                Expect('[');
                Array array;
                SkipWhitespace();
                if (TryConsume(']'))
                {
                    return array;
                }

                while (true)
                {
                    array.push_back(ParseValue());
                    SkipWhitespace();
                    if (TryConsume(']'))
                    {
                        break;
                    }

                    Expect(',');
                }

                return array;
            }

            std::string ParseString()
            {
                Expect('"');
                std::string result;
                while (index_ < input_.size())
                {
                    const char ch = input_[index_++];
                    if (ch == '"')
                    {
                        return result;
                    }

                    if (ch == '\\')
                    {
                        if (index_ >= input_.size())
                        {
                            throw std::runtime_error("Unexpected end of JSON escape.");
                        }

                        const char escaped = input_[index_++];
                        switch (escaped)
                        {
                        case '"': result.push_back('"'); break;
                        case '\\': result.push_back('\\'); break;
                        case '/': result.push_back('/'); break;
                        case 'b': result.push_back('\b'); break;
                        case 'f': result.push_back('\f'); break;
                        case 'n': result.push_back('\n'); break;
                        case 'r': result.push_back('\r'); break;
                        case 't': result.push_back('\t'); break;
                        default: throw std::runtime_error("Unsupported JSON escape.");
                        }
                    }
                    else
                    {
                        result.push_back(ch);
                    }
                }

                throw std::runtime_error("Unterminated JSON string.");
            }

            double ParseNumber()
            {
                const size_t start = index_;
                while (index_ < input_.size())
                {
                    const char ch = input_[index_];
                    if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E'))
                    {
                        break;
                    }

                    ++index_;
                }

                return std::stod(input_.substr(start, index_ - start));
            }

            void ConsumeLiteral(const char* literal)
            {
                while (*literal != '\0')
                {
                    Expect(*literal++);
                }
            }

            void SkipWhitespace()
            {
                while (index_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[index_])))
                {
                    ++index_;
                }
            }

            void Expect(char expected)
            {
                if (index_ >= input_.size() || input_[index_] != expected)
                {
                    throw std::runtime_error("Unexpected JSON token.");
                }

                ++index_;
            }

            bool TryConsume(char expected)
            {
                if (index_ < input_.size() && input_[index_] == expected)
                {
                    ++index_;
                    return true;
                }

                return false;
            }

            const std::string& input_;
            size_t index_{0};
        };

        std::string Escape(const std::string& value)
        {
            std::ostringstream stream;
            for (const auto ch : value)
            {
                switch (ch)
                {
                case '"': stream << "\\\""; break;
                case '\\': stream << "\\\\"; break;
                case '\n': stream << "\\n"; break;
                case '\r': stream << "\\r"; break;
                case '\t': stream << "\\t"; break;
                default: stream << ch; break;
                }
            }

            return stream.str();
        }

        void WriteValue(const Value& value, std::ostringstream& stream, int indentSize, int depth)
        {
            if (std::holds_alternative<std::nullptr_t>(value.data))
            {
                stream << "null";
            }
            else if (std::holds_alternative<bool>(value.data))
            {
                stream << (std::get<bool>(value.data) ? "true" : "false");
            }
            else if (std::holds_alternative<double>(value.data))
            {
                stream << std::setprecision(15) << std::get<double>(value.data);
            }
            else if (std::holds_alternative<std::string>(value.data))
            {
                stream << '"' << Escape(std::get<std::string>(value.data)) << '"';
            }
            else if (std::holds_alternative<Array>(value.data))
            {
                const auto& array = std::get<Array>(value.data);
                stream << "[";
                if (!array.empty())
                {
                    stream << "\n";
                    for (size_t i = 0; i < array.size(); ++i)
                    {
                        stream << std::string((depth + 1) * indentSize, ' ');
                        WriteValue(array[i], stream, indentSize, depth + 1);
                        if (i + 1 < array.size())
                        {
                            stream << ",";
                        }
                        stream << "\n";
                    }
                    stream << std::string(depth * indentSize, ' ');
                }
                stream << "]";
            }
            else
            {
                const auto& object = std::get<Object>(value.data);
                stream << "{";
                if (!object.empty())
                {
                    stream << "\n";
                    size_t index = 0;
                    for (const auto& [key, child] : object)
                    {
                        stream << std::string((depth + 1) * indentSize, ' ') << '"' << Escape(key) << "\": ";
                        WriteValue(child, stream, indentSize, depth + 1);
                        if (++index < object.size())
                        {
                            stream << ",";
                        }
                        stream << "\n";
                    }
                    stream << std::string(depth * indentSize, ' ');
                }
                stream << "}";
            }
        }
    }

    Value Parse(const std::string& input)
    {
        Parser parser(input);
        return parser.ParseValue();
    }

    std::string Serialize(const Value& value, int indentSize)
    {
        std::ostringstream stream;
        WriteValue(value, stream, indentSize, 0);
        return stream.str();
    }
}
