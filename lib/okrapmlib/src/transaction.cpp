#include "okrapmlib/transaction.h"
#include <sstream>
#include <iomanip>

namespace okrapm {

Transaction::Transaction(std::vector<Operation> ops)
    : operations_(std::move(ops)) {}

std::string Transaction::state_name(TransactionState state) {
    switch (state) {
        case TransactionState::Pending:    return "pending";
        case TransactionState::Resolved:   return "resolved";
        case TransactionState::Planned:    return "planned";
        case TransactionState::Verified:   return "verified";
        case TransactionState::Committing: return "committing";
        case TransactionState::Committed:  return "committed";
        case TransactionState::Failed:     return "failed";
        case TransactionState::RolledBack: return "rolled_back";
    }
    return "unknown";
}

std::vector<Operation> Transaction::install_ops() const {
    std::vector<Operation> result;
    for (const auto& op : operations_) {
        if (op.type() == OperationType::Install) result.push_back(op);
    }
    return result;
}

std::vector<Operation> Transaction::remove_ops() const {
    std::vector<Operation> result;
    for (const auto& op : operations_) {
        if (op.type() == OperationType::Remove || op.type() == OperationType::Purge) {
            result.push_back(op);
        }
    }
    return result;
}

std::vector<Operation> Transaction::update_ops() const {
    std::vector<Operation> result;
    for (const auto& op : operations_) {
        if (op.type() == OperationType::Update || op.type() == OperationType::Upgrade) {
            result.push_back(op);
        }
    }
    return result;
}

Transaction::Summary Transaction::summary() const {
    Summary s;
    for (const auto& op : operations_) {
        switch (op.type()) {
            case OperationType::Install: s.install_count++; break;
            case OperationType::Remove:
            case OperationType::Purge:   s.remove_count++; break;
            case OperationType::Update:
            case OperationType::Upgrade: s.update_count++; break;
            default: break;
        }
    }
    // Estimate sizes (placeholder heuristics)
    if (s.install_count > 0 || s.update_count > 0) {
        size_t dl_mb = (s.install_count + s.update_count) * 15; // rough ~15MB per package
        s.download_size = std::to_string(dl_mb) + " MB";
        s.disk_size = "+" + std::to_string(dl_mb * 3) + " MB";
    } else if (s.remove_count > 0) {
        s.download_size = "0 MB";
        s.disk_size = "-" + std::to_string(s.remove_count * 45) + " MB";
    }
    return s;
}

std::string Transaction::to_string() const {
    std::ostringstream oss;
    oss << "Transaction #" << id_ << " [" << state_name(state_) << "]\n";
    if (!description_.empty()) {
        oss << "Description: " << description_ << "\n";
    }
    oss << "Operations (" << operations_.size() << "):\n";
    for (const auto& op : operations_) {
        oss << "  " << op.to_string() << "\n";
    }
    auto sum = summary();
    oss << "\nSummary:\n";
    if (sum.install_count > 0) oss << "  + Install: " << sum.install_count << " package(s)\n";
    if (sum.update_count > 0) oss << "  ~ Update:  " << sum.update_count << " package(s)\n";
    if (sum.remove_count > 0) oss << "  - Remove:  " << sum.remove_count << " package(s)\n";
    if (!sum.download_size.empty()) oss << "  Download:  " << sum.download_size << "\n";
    if (!sum.disk_size.empty())     oss << "  Disk:      " << sum.disk_size << "\n";
    return oss.str();
}

std::string Transaction::serialize() const {
    std::string result;
    result += "id=" + std::to_string(id_) + "\n";
    result += "state=" + state_name(state_) + "\n";
    result += "description=" + description_ + "\n";
    result += "error_message=" + error_message_ + "\n";

    auto time_t_val = std::chrono::system_clock::to_time_t(timestamp_);
    result += "timestamp=" + std::to_string(time_t_val) + "\n";

    result += "operations_count=" + std::to_string(operations_.size()) + "\n";
    for (size_t i = 0; i < operations_.size(); ++i) {
        result += "--op_start--\n";
        result += operations_[i].serialize();
        result += "--op_end--\n";
    }

    return result;
}

std::optional<Transaction> Transaction::deserialize(const std::string& data) {
    Transaction txn;
    std::istringstream iss(data);
    std::string line;
    bool in_op = false;
    std::string op_chunk;

    while (std::getline(iss, line)) {
        if (line == "--op_start--") {
            in_op = true;
            op_chunk.clear();
            continue;
        }
        if (line == "--op_end--") {
            in_op = false;
            auto op = Operation::deserialize(op_chunk);
            if (op) txn.operations_.push_back(*op);
            continue;
        }
        if (in_op) {
            op_chunk += line + "\n";
            continue;
        }

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;
        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        if (key == "id") {
            txn.id_ = std::stoull(value);
        } else if (key == "description") {
            txn.description_ = value;
        } else if (key == "error_message") {
            txn.error_message_ = value;
        } else if (key == "state") {
            if (value == "pending") txn.state_ = TransactionState::Pending;
            else if (value == "resolved") txn.state_ = TransactionState::Resolved;
            else if (value == "planned") txn.state_ = TransactionState::Planned;
            else if (value == "verified") txn.state_ = TransactionState::Verified;
            else if (value == "committing") txn.state_ = TransactionState::Committing;
            else if (value == "committed") txn.state_ = TransactionState::Committed;
            else if (value == "failed") txn.state_ = TransactionState::Failed;
            else if (value == "rolled_back") txn.state_ = TransactionState::RolledBack;
        } else if (key == "timestamp") {
            auto t = static_cast<std::time_t>(std::stoll(value));
            txn.timestamp_ = std::chrono::system_clock::from_time_t(t);
        }
    }

    return txn;
}

} // namespace okrapm
