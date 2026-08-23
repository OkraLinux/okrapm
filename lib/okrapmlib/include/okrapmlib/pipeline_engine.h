#pragma once

#include "object.h"
#include "collection.h"
#include "transaction.h"
#include "lunar_core.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>

namespace okrapm {

// Pipeline 结果
struct PipelineResult {
    bool success{false};
    std::string output;
    Collection<Object> objects;
    std::optional<Transaction> transaction;
    std::string error_message;
};

// 管道执行阶段类型
enum class StageType {
    Source,    // 数据源: find, search, list, groups
    Filter,    // 过滤: where <pred>
    Transform, // 转换: select, sort, limit, unique, expand
    Sink,      // 终端消费: inspect, count, update, install, remove, plan <cmd>
};

// 管道执行阶段基类
class PipelineStage {
public:
    virtual ~PipelineStage() = default;
    virtual StageType stage_type() const = 0;
    virtual std::string name() const = 0;
};

// PipelineEngine: 负责解析并执行对象流管道表达式
// 如: find "gnu.*" | where outdated | update
// 或: list | where repository=main | sort name | inspect
class PipelineEngine {
public:
    // 解析并执行管道表达式
    static PipelineResult execute(const std::string& pipeline_str, LunarCore& core);

    // 辅助: 分割管道字符串中的各级命令
    static std::vector<std::string> split_pipeline(const std::string& expr);

    // 辅助: 解析命令行参数 (支持引号)
    static std::vector<std::string> parse_tokens(const std::string& stage_str);
};

} // namespace okrapm
