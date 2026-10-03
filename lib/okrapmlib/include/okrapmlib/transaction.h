#pragma once

#include "operation.h"
#include "object.h"
#include <string>
#include <vector>
#include <chrono>
#include <optional>

namespace okrapm {

enum class TransactionState {
    Pending,
    Resolved,
    Planned,
    Verified,
    Committing,
    Committed,
    Failed,
    RolledBack,
};

class Transaction {
public:
    Transaction() = default;

    explicit Transaction(std::vector<Operation> ops);

    uint64_t id() const { return id_; }
    void set_id(uint64_t id) { id_ = id; }

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

    static std::string state_name(TransactionState state);

    const std::vector<Operation>& operations() const { return operations_; }
    std::vector<Operation>& operations() { return operations_; }
    void add_operation(Operation op) { operations_.push_back(std::move(op)); }

    std::vector<Operation> install_ops() const;

    std::vector<Operation> remove_ops() const;

    std::vector<Operation> update_ops() const;

    std::chrono::system_clock::time_point timestamp() const { return timestamp_; }
    void set_timestamp(std::chrono::system_clock::time_point ts) { timestamp_ = ts; }

    struct Summary {
        size_t install_count{0};
        size_t remove_count{0};
        size_t update_count{0};
        std::string download_size;
        std::string disk_size;
    };
    Summary summary() const;

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

}
