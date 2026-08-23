#pragma once

#include "operation.h"
#include "object.h"
#include <string>
#include <vector>
#include <chrono>
#include <optional>

namespace okrapm {

// 事务状态生命周期:
//   pending -> resolved -> planned -> verified -> committing -> committed
//   失败时: committing -> failed -> rollback
enum class TransactionState {
    Pending,        // 刚创建，未解析
    Resolved,       // 依赖已解析
    Planned,        // 事务计划已生成
    Verified,       // 计划已验证
    Committing,     // 正在提交
    Committed,      // 已提交
    Failed,         // 提交失败
    RolledBack,     // 已回滚
};

// Transaction: 事务
// 任何修改系统状态的操作都形成 Transaction
// 事务有完整生命周期: pending -> resolved -> planned -> verified -> committing -> committed
class Transaction {
public:
    Transaction() = default;

    // 从操作列表创建事务
    explicit Transaction(std::vector<Operation> ops);

    // 事务 ID
    uint64_t id() const { return id_; }
    void set_id(uint64_t id) { id_ = id; }

    // 状态管理
    TransactionState state() const { return state_; }
    void set_state(TransactionState state) { state_ = state; }
    void advance_state(TransactionState state, const std::string& error = "") {
        state_ = state;
        if (!error.empty()) error_message_ = error;
    }

    const std::string& description() const { return description_; }
    void set_description(const std::string& desc) { description_ = desc; }

    const std::string& error_message() const { return error_message_; }
    void set_error_message(const std::string& err) { error_message_ = err; }

    std::string to_string() const;

    // 状态名称
    static std::string state_name(TransactionState state);

    // 操作列表
    const std::vector<Operation>& operations() const { return operations_; }
    std::vector<Operation>& operations() { return operations_; }
    void add_operation(Operation op) { operations_.push_back(std::move(op)); }

    // 获取安装操作
    std::vector<Operation> install_ops() const;
    // 获取删除操作
    std::vector<Operation> remove_ops() const;
    // 获取更新操作
    std::vector<Operation> update_ops() const;

    // 时间戳
    std::chrono::system_clock::time_point timestamp() const { return timestamp_; }
    void set_timestamp(std::chrono::system_clock::time_point ts) { timestamp_ = ts; }

    // 事务摘要 (用于 plan 预览)
    struct Summary {
        size_t install_count{0};
        size_t remove_count{0};
        size_t update_count{0};
        std::string download_size;   // 下载大小估算
        std::string disk_size;       // 磁盘占用变化估算
    };
    Summary summary() const;

    // 序列化/反序列化
    std::string serialize() const;
    static std::optional<Transaction> deserialize(const std::string& data);

private:
    uint64_t id_{0};
    std::string description_;
    std::string error_message_;
    TransactionState state_{TransactionState::Pending};
    std::vector<Operation> operations_;
    std::chrono::system_clock::time_point timestamp_{std::chrono::system_clock::now()};
};

} // namespace okrapm
