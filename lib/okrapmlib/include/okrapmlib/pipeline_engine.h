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

struct PipelineResult {
    bool success{false};
    std::string output;
    Collection<Object> objects;
    std::optional<Transaction> transaction;
    std::string error_message;
};

enum class StageType {
    Source,
    Filter,
    Transform,
    Sink,
};

class PipelineStage {
public:
    virtual ~PipelineStage() = default;
    virtual StageType stage_type() const = 0;
    virtual std::string name() const = 0;
};

class PipelineEngine {
public:

    static PipelineResult execute(const std::string& pipeline_str, LunarCore& core);

    static std::vector<std::string> split_pipeline(const std::string& expr);

    static std::vector<std::string> parse_tokens(const std::string& stage_str);
};

}
