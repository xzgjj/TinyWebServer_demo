#pragma once

#include <string>
#include <map>
#include <vector>
#include <optional>
#include <stdexcept>

namespace tinywebserver {
namespace config {

class JsonValue;

class JsonObject {
public:
    bool HasKey(const std::string& key) const {
        return values_.find(key) != values_.end();
    }

    template<typename T>
    T Get(const std::string& key, const T& default_value) const {
        auto it = values_.find(key);
        if (it == values_.end()) {
            return default_value;
        }
        return it->second.As<T>();
    }

    template<typename T>
    std::optional<T> GetOptional(const std::string& key) const {
        auto it = values_.find(key);
        if (it == values_.end()) {
            return std::nullopt;
        }
        return it->second.As<T>();
    }

    void Set(const std::string& key, const JsonValue& value) {
        values_[key] = value;
    }

private:
    std::map<std::string, JsonValue> values_;
    friend class JsonParser;
};

class JsonValue {
public:
    enum class Type {
        Null,
        Boolean,
        Integer,
        Double,
        String,
        Object,
        Array
    };

    JsonValue() : type_(Type::Null) {}
    JsonValue(bool b) : type_(Type::Boolean), bool_value_(b) {}
    JsonValue(int i) : type_(Type::Integer), int_value_(i) {}
    JsonValue(double d) : type_(Type::Double), double_value_(d) {}
    JsonValue(const std::string& s) : type_(Type::String), string_value_(s) {}
    JsonValue(const JsonObject& obj) : type_(Type::Object), object_value_(new JsonObject(obj)) {}
    JsonValue(const std::vector<JsonValue>& arr) : type_(Type::Array), array_value_(arr) {}

    Type GetType() const { return type_; }

    template<typename T>
    T As() const {
        throw std::runtime_error("Type conversion not implemented");
    }

    template<>
    bool As<bool>() const {
        if (type_ != Type::Boolean) {
            throw std::runtime_error("Not a boolean");
        }
        return bool_value_;
    }

    template<>
    int As<int>() const {
        if (type_ != Type::Integer) {
            throw std::runtime_error("Not an integer");
        }
        return int_value_;
    }

    template<>
    double As<double>() const {
        if (type_ == Type::Integer) {
            return static_cast<double>(int_value_);
        }
        if (type_ != Type::Double) {
            throw std::runtime_error("Not a double");
        }
        return double_value_;
    }

    template<>
    std::string As<std::string>() const {
        if (type_ != Type::String) {
            throw std::runtime_error("Not a string");
        }
        return string_value_;
    }

    template<>
    JsonObject As<JsonObject>() const {
        if (type_ != Type::Object) {
            throw std::runtime_error("Not an object");
        }
        return *object_value_;
    }

private:
    Type type_;
    union {
        bool bool_value_;
        int int_value_;
        double double_value_;
    };
    std::string string_value_;
    std::shared_ptr<JsonObject> object_value_;
    std::vector<JsonValue> array_value_;
};

class JsonParser {
public:
    static JsonObject Parse(const std::string& json_str) {
        // 简化实现：仅解析简单对象
        // TODO: 实现完整 JSON 解析
        JsonObject obj;
        // 临时实现：返回空对象
        return obj;
    }

    static JsonObject ParseFile(const std::string& filename) {
        // 读取文件并解析
        // TODO: 实现文件读取
        return JsonObject();
    }
};

} // namespace config
} // namespace tinywebserver