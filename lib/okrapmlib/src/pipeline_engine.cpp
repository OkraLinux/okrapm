#include "okrapmlib/pipeline_engine.h"
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace okrapm {

static std::string trim_str(const std::string& str) {
    auto first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    auto last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::vector<std::string> PipelineEngine::split_pipeline(const std::string& expr) {
    std::vector<std::string> stages;
    std::string current;
    bool in_quote = false;
    char quote_char = 0;

    for (size_t i = 0; i < expr.size(); ++i) {
        char c = expr[i];
        if ((c == '"' || c == '\'') && (i == 0 || expr[i - 1] != '\\')) {
            if (in_quote && c == quote_char) {
                in_quote = false;
                quote_char = 0;
            } else if (!in_quote) {
                in_quote = true;
                quote_char = c;
            }
            current += c;
        } else if (c == '|' && !in_quote) {
            std::string s = trim_str(current);
            if (!s.empty()) stages.push_back(s);
            current.clear();
        } else {
            current += c;
        }
    }
    std::string s = trim_str(current);
    if (!s.empty()) stages.push_back(s);
    return stages;
}

std::vector<std::string> PipelineEngine::parse_tokens(const std::string& stage_str) {
    std::vector<std::string> tokens;
    std::string token;
    bool in_quote = false;
    char quote_char = 0;

    for (size_t i = 0; i < stage_str.size(); ++i) {
        char c = stage_str[i];
        if ((c == '"' || c == '\'') && (i == 0 || stage_str[i - 1] != '\\')) {
            if (in_quote && c == quote_char) {
                in_quote = false;
                quote_char = 0;
            } else if (!in_quote) {
                in_quote = true;
                quote_char = c;
            }
        } else if ((c == ' ' || c == '\t') && !in_quote) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    if (!token.empty()) {
        tokens.push_back(token);
    }
    return tokens;
}

PipelineResult PipelineEngine::execute(const std::string& pipeline_str, LunarCore& core) {
    PipelineResult res;
    auto stages = split_pipeline(pipeline_str);
    if (stages.empty()) {
        res.error_message = "Empty pipeline expression";
        return res;
    }

    Collection<Object> stream;
    bool has_source = false;

    for (size_t stage_idx = 0; stage_idx < stages.size(); ++stage_idx) {
        auto tokens = parse_tokens(stages[stage_idx]);
        if (tokens.empty()) continue;

        std::string cmd = tokens[0];

        // ---- 1. Source Stages ----
        if (cmd == "find" || cmd == "search" || cmd == "list" || cmd == "groups" || cmd == "installed") {
            has_source = true;
            if (cmd == "find") {
                std::string pattern = (tokens.size() > 1) ? tokens[1] : "*";
                stream = core.find(pattern);
            } else if (cmd == "search") {
                std::string query = (tokens.size() > 1) ? tokens[1] : "";
                stream = core.search(query);
            } else if (cmd == "list" || cmd == "installed") {
                stream = core.list_installed();
            } else if (cmd == "groups") {
                auto all = core.find("*");
                stream = all.where([](const Object& o) { return o.type() == ObjectType::Group; });
            }
            continue;
        }

        if (!has_source) {
            // 如果第一个阶段不是显式 source，默认以已安装对象为 source
            stream = core.list_installed();
            has_source = true;
        }

        // ---- 2. Filter Stage (where) ----
        if (cmd == "where" || cmd == "filter") {
            if (tokens.size() < 2) {
                res.error_message = "Syntax error: 'where' expects a condition (e.g. outdated, installed, repository=main)";
                return res;
            }
            std::string condition = tokens[1];

            if (condition == "outdated") {
                // 判断哪些已安装的包有更新版本
                auto all_available = core.find("*");
                std::unordered_map<std::string, Version> latest_versions;
                for (const auto& avail : all_available.to_vector()) {
                    std::string key = avail.ns() + "." + avail.name();
                    auto it = latest_versions.find(key);
                    if (it == latest_versions.end() || avail.version() > it->second) {
                        latest_versions[key] = avail.version();
                    }
                }

                stream = stream.where([&](const Object& o) {
                    if (o.state() == ObjectState::Outdated) return true;
                    std::string key = o.ns() + "." + o.name();
                    auto it = latest_versions.find(key);
                    return (it != latest_versions.end() && it->second > o.version());
                });
            } else if (condition == "installed") {
                stream = stream.where(where_installed());
            } else if (condition.rfind("repository=", 0) == 0 || condition.rfind("repo=", 0) == 0) {
                auto eq_pos = condition.find('=');
                std::string repo_name = condition.substr(eq_pos + 1);
                stream = stream.where(where_repository(repo_name));
            } else if (condition.rfind("namespace=", 0) == 0 || condition.rfind("ns=", 0) == 0) {
                auto eq_pos = condition.find('=');
                std::string ns_name = condition.substr(eq_pos + 1);
                stream = stream.where(where_namespace(ns_name));
            } else if (condition.rfind("type=", 0) == 0) {
                auto eq_pos = condition.find('=');
                std::string type_str = condition.substr(eq_pos + 1);
                ObjectType t = ObjectType::Package;
                if (type_str == "group") t = ObjectType::Group;
                else if (type_str == "system") t = ObjectType::System;
                else if (type_str == "artifact") t = ObjectType::Artifact;
                stream = stream.where(where_type(t));
            } else if (condition.rfind("name=", 0) == 0) {
                auto eq_pos = condition.find('=');
                std::string pat = condition.substr(eq_pos + 1);
                stream = stream.where([pat](const Object& o) {
                    return ObjectRef::matches_pattern(pat, o.name()) ||
                           ObjectRef::matches_pattern(pat, o.full_name());
                });
            } else {
                // 默认按 pattern 匹配
                stream = stream.where([condition](const Object& o) {
                    return ObjectRef::matches_pattern(condition, o.full_name()) ||
                           ObjectRef::matches_pattern(condition, o.name());
                });
            }
            continue;
        }

        // ---- 3. Transform Stages ----
        if (cmd == "sort") {
            std::string field = (tokens.size() > 1) ? tokens[1] : "name";
            if (field == "name") {
                stream = stream.sort([](const Object& a, const Object& b) {
                    return a.full_name() < b.full_name();
                });
            } else if (field == "version") {
                stream = stream.sort([](const Object& a, const Object& b) {
                    return a.version() < b.version();
                });
            } else if (field == "size" || field == "installed_size") {
                stream = stream.sort([](const Object& a, const Object& b) {
                    return a.installed_size() > b.installed_size();
                });
            }
            continue;
        }

        if (cmd == "limit") {
            size_t n = (tokens.size() > 1) ? std::stoull(tokens[1]) : 10;
            stream = stream.limit(n);
            continue;
        }

        if (cmd == "unique") {
            stream = stream.unique();
            continue;
        }

        if (cmd == "expand") {
            stream = stream.expand([&](const Object& item) -> std::vector<Object> {
                if (item.type() == ObjectType::Group) {
                    std::vector<Object> members;
                    for (const auto& member_ref : item.dependencies()) {
                        auto mem_obj = core.info(member_ref);
                        if (mem_obj) {
                            members.push_back(*mem_obj);
                        } else {
                            members.push_back(Object("unknown", member_ref));
                        }
                    }
                    return members;
                }
                return {item};
            });
            continue;
        }

        // ---- 4. Sink Stages ----
        if (cmd == "inspect") {
            std::ostringstream oss;
            oss << "Collection<Object> (" << stream.count() << " items):\n";
            for (const auto& obj : stream.to_vector()) {
                oss << "  " << std::left << std::setw(28) << obj.ref_string()
                    << " [" << Object::type_name(obj.type()) << "] "
                    << "(" << obj.repository() << ") - "
                    << obj.description() << "\n";
            }
            res.output = oss.str();
            res.objects = stream;
            res.success = true;
            return res;
        }

        if (cmd == "count") {
            res.output = std::to_string(stream.count()) + "\n";
            res.objects = stream;
            res.success = true;
            return res;
        }

        if (cmd == "update") {
            std::vector<std::string> refs;
            for (const auto& obj : stream.to_vector()) {
                refs.push_back(obj.full_name());
            }
            if (refs.empty()) {
                res.output = ":: No objects in stream to update.\n";
                res.success = true;
                return res;
            }
            auto update_res = core.update(refs, /*plan_only=*/false);
            res.success = update_res.success;
            res.transaction = update_res.transaction;
            res.error_message = update_res.error_message;
            if (res.success) {
                res.output = update_res.transaction.to_string() + "\n:: Pipeline update completed successfully.\n";
            }
            return res;
        }

        if (cmd == "install") {
            std::vector<std::string> refs;
            for (const auto& obj : stream.to_vector()) {
                refs.push_back(obj.ref_string());
            }
            if (refs.empty()) {
                res.output = ":: No objects in stream to install.\n";
                res.success = true;
                return res;
            }
            auto inst_res = core.install(refs, /*plan_only=*/false);
            res.success = inst_res.success;
            res.transaction = inst_res.transaction;
            res.error_message = inst_res.error_message;
            if (res.success) {
                res.output = inst_res.transaction.to_string() + "\n:: Pipeline install completed successfully.\n";
            }
            return res;
        }

        if (cmd == "remove") {
            std::vector<std::string> refs;
            for (const auto& obj : stream.to_vector()) {
                refs.push_back(obj.full_name());
            }
            if (refs.empty()) {
                res.output = ":: No objects in stream to remove.\n";
                res.success = true;
                return res;
            }
            auto rem_res = core.remove(refs, /*purge=*/false, /*plan_only=*/false);
            res.success = rem_res.success;
            res.transaction = rem_res.transaction;
            res.error_message = rem_res.error_message;
            if (res.success) {
                res.output = rem_res.transaction.to_string() + "\n:: Pipeline remove completed successfully.\n";
            }
            return res;
        }

        if (cmd == "plan") {
            if (tokens.size() < 2) {
                res.error_message = "Syntax error: 'plan' expects a sub-command (e.g. plan update, plan install, plan remove)";
                return res;
            }
            std::string sub = tokens[1];
            std::vector<std::string> refs;
            for (const auto& obj : stream.to_vector()) {
                refs.push_back(obj.ref_string());
            }

            if (sub == "update") {
                auto pres = core.update(refs, /*plan_only=*/true);
                res.success = pres.success;
                res.transaction = pres.transaction;
                res.output = pres.transaction.to_string() + "\n";
                return res;
            } else if (sub == "install") {
                auto pres = core.install(refs, /*plan_only=*/true);
                res.success = pres.success;
                res.transaction = pres.transaction;
                res.output = pres.transaction.to_string() + "\n";
                return res;
            } else if (sub == "remove") {
                auto pres = core.remove(refs, /*purge=*/false, /*plan_only=*/true);
                res.success = pres.success;
                res.transaction = pres.transaction;
                res.output = pres.transaction.to_string() + "\n";
                return res;
            } else {
                res.error_message = "Unknown plan sub-command: " + sub;
                return res;
            }
        }

        res.error_message = "Unknown pipeline stage command: " + cmd;
        return res;
    }

    // 若末尾没有显式 sink，则默认输出 inspect 格式
    std::ostringstream oss;
    oss << "Collection<Object> (" << stream.count() << " items):\n";
    for (const auto& obj : stream.to_vector()) {
        oss << "  " << std::left << std::setw(28) << obj.ref_string()
            << " [" << Object::type_name(obj.type()) << "] "
            << "(" << obj.repository() << ") - "
            << obj.description() << "\n";
    }
    res.output = oss.str();
    res.objects = stream;
    res.success = true;
    return res;
}

} // namespace okrapm
