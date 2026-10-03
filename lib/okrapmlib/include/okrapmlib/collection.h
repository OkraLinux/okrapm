#pragma once

#include "object.h"
#include "object_ref.h"
#include <vector>
#include <functional>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <optional>

namespace okrapm {

template<typename T>
class Collection {
public:
    Collection() = default;
    explicit Collection(std::vector<T> items) : items_(std::move(items)) {}

    const std::vector<T>& items() const { return items_; }
    const std::vector<T>& to_vector() const { return items_; }
    size_t size() const { return items_.size(); }
    size_t count() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    const T& at(size_t i) const { return items_.at(i); }

    Collection<T> where(std::function<bool(const T&)> predicate) const {
        std::vector<T> result;
        for (const auto& item : items_) {
            if (predicate(item)) {
                result.push_back(item);
            }
        }
        return Collection<T>(std::move(result));
    }

    template<typename U>
    Collection<U> select(std::function<U(const T&)> transform) const {
        std::vector<U> result;
        result.reserve(items_.size());
        for (const auto& item : items_) {
            result.push_back(transform(item));
        }
        return Collection<U>(std::move(result));
    }

    Collection<T> sort(std::function<bool(const T&, const T&)> comparator = nullptr) const {
        std::vector<T> result = items_;
        if (comparator) {
            std::sort(result.begin(), result.end(), comparator);
        } else {
            std::sort(result.begin(), result.end());
        }
        return Collection<T>(std::move(result));
    }

    Collection<T> unique() const {
        std::vector<T> result;
        std::unordered_set<std::string> seen;
        for (const auto& item : items_) {
            std::string key = item.ns() + "." + item.name();
            if (seen.find(key) == seen.end()) {
                seen.insert(key);
                result.push_back(item);
            }
        }
        return Collection<T>(std::move(result));
    }

    Collection<T> limit(size_t n) const {
        std::vector<T> result;
        size_t count = std::min(n, items_.size());
        for (size_t i = 0; i < count; ++i) {
            result.push_back(items_[i]);
        }
        return Collection<T>(std::move(result));
    }

    std::vector<Collection<T>> group_by(std::function<std::string(const T&)> key_fn) const {
        std::vector<std::pair<std::string, std::vector<T>>> buckets;
        for (const auto& item : items_) {
            std::string key = key_fn(item);
            bool found = false;
            for (auto& bucket : buckets) {
                if (bucket.first == key) {
                    bucket.second.push_back(item);
                    found = true;
                    break;
                }
            }
            if (!found) {
                buckets.push_back({key, {item}});
            }
        }
        std::vector<Collection<T>> result;
        for (auto& bucket : buckets) {
            result.push_back(Collection<T>(std::move(bucket.second)));
        }
        return result;
    }

    Collection<T> expand(std::function<std::vector<T>(const T&)> expander) const {
        std::vector<T> result;
        for (const auto& item : items_) {
            auto expanded = expander(item);
            for (auto& e : expanded) {
                result.push_back(std::move(e));
            }
        }
        return Collection<T>(std::move(result));
    }

    Collection<T> inspect(std::function<void(const T&)> fn) const {
        for (const auto& item : items_) {
            fn(item);
        }
        return *this;
    }

    Collection<T> filter(std::function<bool(const T&)> predicate) const {
        return where(predicate);
    }

    bool any(std::function<bool(const T&)> predicate) const {
        for (const auto& item : items_) {
            if (predicate(item)) return true;
        }
        return false;
    }

    bool all(std::function<bool(const T&)> predicate) const {
        for (const auto& item : items_) {
            if (!predicate(item)) return false;
        }
        return true;
    }

    std::optional<T> first() const {
        if (items_.empty()) return std::nullopt;
        return items_[0];
    }

    Collection<T> concat(const Collection<T>& other) const {
        std::vector<T> result = items_;
        for (const auto& item : other.items_) {
            result.push_back(item);
        }
        return Collection<T>(std::move(result));
    }

private:
    std::vector<T> items_;
};

inline std::function<bool(const Object&)> where_outdated() {
    return [](const Object& obj) { return obj.state() == ObjectState::Outdated; };
}

inline std::function<bool(const Object&)> where_installed() {
    return [](const Object& obj) {
        return obj.state() == ObjectState::Installed || obj.state() == ObjectState::Outdated;
    };
}

inline std::function<bool(const Object&)> where_repository(const std::string& repo) {
    return [repo](const Object& obj) { return obj.repository() == repo; };
}

inline std::function<bool(const Object&)> where_namespace(const std::string& ns) {
    return [ns](const Object& obj) { return obj.ns() == ns; };
}

inline std::function<bool(const Object&)> where_type(ObjectType type) {
    return [type](const Object& obj) { return obj.type() == type; };
}

}
