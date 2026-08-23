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

// Collection<T>: 强类型对象集合引擎
// 集合操作返回新的 Collection，支持链式调用
// 这不是打印到终端的文本，而是 Lunar 内部可继续处理的强类型数据
template<typename T>
class Collection {
public:
    Collection() = default;
    explicit Collection(std::vector<T> items) : items_(std::move(items)) {}

    // 基本访问
    const std::vector<T>& items() const { return items_; }
    const std::vector<T>& to_vector() const { return items_; }
    size_t size() const { return items_.size(); }
    size_t count() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    const T& at(size_t i) const { return items_.at(i); }

    // ---- 集合操作 ----

    // where: 过滤集合，保留满足谓词的元素
    Collection<T> where(std::function<bool(const T&)> predicate) const {
        std::vector<T> result;
        for (const auto& item : items_) {
            if (predicate(item)) {
                result.push_back(item);
            }
        }
        return Collection<T>(std::move(result));
    }

    // select: 转换每个元素 (映射)
    template<typename U>
    Collection<U> select(std::function<U(const T&)> transform) const {
        std::vector<U> result;
        result.reserve(items_.size());
        for (const auto& item : items_) {
            result.push_back(transform(item));
        }
        return Collection<U>(std::move(result));
    }

    // sort: 按比较器排序
    Collection<T> sort(std::function<bool(const T&, const T&)> comparator = nullptr) const {
        std::vector<T> result = items_;
        if (comparator) {
            std::sort(result.begin(), result.end(), comparator);
        } else {
            std::sort(result.begin(), result.end());
        }
        return Collection<T>(std::move(result));
    }

    // unique: 去重 (基于名称)
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

    // limit: 取前 n 个元素
    Collection<T> limit(size_t n) const {
        std::vector<T> result;
        size_t count = std::min(n, items_.size());
        for (size_t i = 0; i < count; ++i) {
            result.push_back(items_[i]);
        }
        return Collection<T>(std::move(result));
    }

    // group: 按键分组
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

    // expand: 将 Group 展开为 Package 集合
    // 需要一个展开函数来处理 Group -> 多个 Package 的映射
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

    // inspect: 对每个元素执行一个操作 (主要用于打印/调试)
    Collection<T> inspect(std::function<void(const T&)> fn) const {
        for (const auto& item : items_) {
            fn(item);
        }
        return *this;
    }

    // filter alias for where
    Collection<T> filter(std::function<bool(const T&)> predicate) const {
        return where(predicate);
    }

    // any: 是否有元素满足谓词
    bool any(std::function<bool(const T&)> predicate) const {
        for (const auto& item : items_) {
            if (predicate(item)) return true;
        }
        return false;
    }

    // all: 是否所有元素都满足谓词
    bool all(std::function<bool(const T&)> predicate) const {
        for (const auto& item : items_) {
            if (!predicate(item)) return false;
        }
        return true;
    }

    // first: 取第一个元素 (如果有)
    std::optional<T> first() const {
        if (items_.empty()) return std::nullopt;
        return items_[0];
    }

    // concat: 连接两个集合
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

// ---- 预定义的 where 谓词 ----

// 筛选过期对象
inline std::function<bool(const Object&)> where_outdated() {
    return [](const Object& obj) { return obj.state() == ObjectState::Outdated; };
}

// 筛选已安装对象
inline std::function<bool(const Object&)> where_installed() {
    return [](const Object& obj) {
        return obj.state() == ObjectState::Installed || obj.state() == ObjectState::Outdated;
    };
}

// 筛选指定仓库的对象
inline std::function<bool(const Object&)> where_repository(const std::string& repo) {
    return [repo](const Object& obj) { return obj.repository() == repo; };
}

// 筛选指定命名空间的对象
inline std::function<bool(const Object&)> where_namespace(const std::string& ns) {
    return [ns](const Object& obj) { return obj.ns() == ns; };
}

// 筛选指定类型的对象
inline std::function<bool(const Object&)> where_type(ObjectType type) {
    return [type](const Object& obj) { return obj.type() == type; };
}

} // namespace okrapm
