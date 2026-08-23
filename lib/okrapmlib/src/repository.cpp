#include "okrapmlib/repository.h"
#include "okrapmlib/network_downloader.h"
#include "okrapmlib/artifact_engine.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace okrapm {

// ==========================================
// LocalRepository
// ==========================================

LocalRepository::LocalRepository(const std::string& name, const std::string& db_path,
                                 const std::string& url, bool enabled)
    : name_(name), db_path_(db_path), url_(url), enabled_(enabled) {
    load();
}

std::optional<Object> LocalRepository::find(const std::string& ns, const std::string& name) const {
    if (!enabled_) return std::nullopt;
    for (const auto& obj : objects_) {
        if (obj.name() == name && (ns.empty() || obj.ns() == ns)) {
            return obj;
        }
    }
    return std::nullopt;
}

std::optional<Object> LocalRepository::find_by_ref(const ObjectRef& ref) const {
    if (!enabled_) return std::nullopt;
    for (const auto& obj : objects_) {
        if (ref.matches(obj)) {
            return obj;
        }
    }
    return std::nullopt;
}

std::vector<Object> LocalRepository::list_objects() const {
    if (!enabled_) return {};
    return objects_;
}

Collection<Object> LocalRepository::search(const std::string& query) const {
    if (!enabled_) return Collection<Object>();

    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    std::vector<Object> matches;
    for (const auto& obj : objects_) {
        std::string lower_name = obj.full_name();
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

        std::string lower_desc = obj.description();
        std::transform(lower_desc.begin(), lower_desc.end(), lower_desc.begin(), ::tolower);

        if (lower_name.find(lower_query) != std::string::npos ||
            lower_desc.find(lower_query) != std::string::npos) {
            matches.push_back(obj);
        }
    }
    return Collection<Object>(std::move(matches));
}

Collection<Object> LocalRepository::find_pattern(const std::string& pattern) const {
    if (!enabled_) return Collection<Object>();

    auto ref = ObjectRef::parse(pattern);
    if (!ref) return Collection<Object>();

    std::vector<Object> matches;
    for (const auto& obj : objects_) {
        if (ref->matches(obj)) {
            matches.push_back(obj);
        }
    }
    return Collection<Object>(std::move(matches));
}

bool LocalRepository::sync() {
    return load();
}

bool LocalRepository::save() const {
    auto parent = fs::path(db_path_).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }

    std::ofstream ofs(db_path_);
    if (!ofs.is_open()) return false;

    ofs << "# Lunar Local Repository DB: " << name_ << "\n";
    for (const auto& obj : objects_) {
        ofs << obj.serialize() << "\n";
    }
    return true;
}

bool LocalRepository::load() {
    objects_.clear();
    std::ifstream ifs(db_path_);
    if (!ifs.is_open()) return false;

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto obj = Object::deserialize(line);
        if (obj) {
            obj->set_repository(name_);
            objects_.push_back(*obj);
        }
    }
    return true;
}

void LocalRepository::add_object(const Object& obj) {
    Object copy = obj;
    copy.set_repository(name_);
    for (auto& o : objects_) {
        if (o.ns() == copy.ns() && o.name() == copy.name()) {
            o = copy;
            return;
        }
    }
    objects_.push_back(copy);
}

void LocalRepository::remove_object(const std::string& ns, const std::string& name) {
    objects_.erase(
        std::remove_if(objects_.begin(), objects_.end(),
                       [&](const Object& o) {
                           return o.ns() == ns && o.name() == name;
                       }),
        objects_.end()
    );
}

// ==========================================
// RemoteRepository
// ==========================================

RemoteRepository::RemoteRepository(const std::string& name, const std::string& url,
                                   const std::string& cache_dir, bool enabled)
    : name_(name), url_(url), cache_dir_(cache_dir), enabled_(enabled) {
    std::error_code ec;
    fs::create_directories(cache_dir_, ec);
    fs::create_directories(cache_dir_ + "/artifacts", ec);
    load();
}

std::optional<Object> RemoteRepository::find(const std::string& ns, const std::string& name) const {
    if (!enabled_) return std::nullopt;
    for (const auto& obj : objects_) {
        if (obj.name() == name && (ns.empty() || obj.ns() == ns)) {
            return obj;
        }
    }
    return std::nullopt;
}

std::optional<Object> RemoteRepository::find_by_ref(const ObjectRef& ref) const {
    if (!enabled_) return std::nullopt;
    for (const auto& obj : objects_) {
        if (ref.matches(obj)) {
            return obj;
        }
    }
    return std::nullopt;
}

std::vector<Object> RemoteRepository::list_objects() const {
    if (!enabled_) return {};
    return objects_;
}

Collection<Object> RemoteRepository::search(const std::string& query) const {
    if (!enabled_) return Collection<Object>();

    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    std::vector<Object> matches;
    for (const auto& obj : objects_) {
        std::string lower_name = obj.full_name();
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

        std::string lower_desc = obj.description();
        std::transform(lower_desc.begin(), lower_desc.end(), lower_desc.begin(), ::tolower);

        if (lower_name.find(lower_query) != std::string::npos ||
            lower_desc.find(lower_query) != std::string::npos) {
            matches.push_back(obj);
        }
    }
    return Collection<Object>(std::move(matches));
}

Collection<Object> RemoteRepository::find_pattern(const std::string& pattern) const {
    if (!enabled_) return Collection<Object>();

    auto ref = ObjectRef::parse(pattern);
    if (!ref) return Collection<Object>();

    std::vector<Object> matches;
    for (const auto& obj : objects_) {
        if (ref->matches(obj)) {
            matches.push_back(obj);
        }
    }
    return Collection<Object>(std::move(matches));
}

bool RemoteRepository::save() const {
    std::string db_path = cache_dir_ + "/index.db";
    std::ofstream ofs(db_path);
    if (!ofs.is_open()) return false;

    ofs << "# Lunar Remote Repository Cache: " << name_ << " (" << url_ << ")\n";
    for (const auto& obj : objects_) {
        ofs << obj.serialize() << "\n";
    }
    return true;
}

bool RemoteRepository::load() {
    objects_.clear();
    std::string db_path = cache_dir_ + "/index.db";
    std::ifstream ifs(db_path);
    if (!ifs.is_open()) return false;

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto obj = Object::deserialize(line);
        if (obj) {
            obj->set_repository(name_);
            objects_.push_back(*obj);
        }
    }
    return true;
}

bool RemoteRepository::sync() {
    if (url_.empty()) return false;

    // 尝试拉取 index.yaml 或 index.db
    std::string index_url = url_ + "/index.yaml";
    auto content_opt = NetworkDownloader::download_string(index_url);
    if (!content_opt) {
        index_url = url_ + "/index.db";
        content_opt = NetworkDownloader::download_string(index_url);
    }
    if (!content_opt) {
        index_url = url_ + "/packages.idx";
        content_opt = NetworkDownloader::download_string(index_url);
    }

    if (!content_opt) {
        std::cerr << "RemoteRepository: Failed to fetch index from " << url_ << "\n";
        return false;
    }

    std::string content = *content_opt;
    std::vector<Object> new_objects;

    // 检查是否为 YAML 块格式
    if (content.find("packages:") != std::string::npos || content.find("name:") != std::string::npos) {
        // 多对象 YAML 解析
        std::istringstream iss(content);
        std::string line;
        std::string current_chunk;

        auto parse_and_add_chunk = [&](const std::string& chunk) {
            if (chunk.empty()) return;
            auto meta = ArtifactMetadata::parse_yaml(chunk);
            if (meta) {
                auto obj = meta->to_object();
                obj.set_repository(name_);
                new_objects.push_back(obj);
            }
        };

        while (std::getline(iss, line)) {
            if (line.rfind("---", 0) == 0) {
                parse_and_add_chunk(current_chunk);
                current_chunk.clear();
            } else {
                current_chunk += line + "\n";
            }
        }
        parse_and_add_chunk(current_chunk);
    } else {
        // 按照单行序列化解析
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto obj = Object::deserialize(line);
            if (obj) {
                obj->set_repository(name_);
                new_objects.push_back(*obj);
            }
        }
    }

    if (!new_objects.empty()) {
        objects_ = std::move(new_objects);
        save();
        return true;
    }

    return false;
}

std::optional<std::string> RemoteRepository::fetch_artifact(const Object& obj, const std::string& dest_dir) {
    std::string target_dir = dest_dir.empty() ? (cache_dir_ + "/artifacts") : dest_dir;
    std::error_code ec;
    fs::create_directories(target_dir, ec);

    // 候选文件名
    std::vector<std::string> filenames = {
        obj.ns() + "." + obj.name() + "@" + obj.version().to_string() + ".oaa",
        obj.name() + "-" + obj.version().to_string() + ".oaa",
        obj.name() + "-" + obj.version().to_string() + ".okra",
        obj.name() + ".oaa"
    };

    for (const auto& fname : filenames) {
        std::string dest_path = target_dir + "/" + fname;
        // 如果本地缓存已存在
        if (fs::exists(dest_path) && fs::file_size(dest_path, ec) > 0) {
            return dest_path;
        }

        // 尝试从远程 URL 候选路径下载
        std::vector<std::string> candidate_urls = {
            url_ + "/artifacts/" + fname,
            url_ + "/" + fname,
            url_ + "/packages/" + fname
        };

        for (const auto& dl_url : candidate_urls) {
            auto res = NetworkDownloader::download_file(dl_url, dest_path);
            if (res.success && fs::exists(dest_path) && fs::file_size(dest_path, ec) > 0) {
                return dest_path;
            }
        }
    }

    return std::nullopt;
}

// ==========================================
// RepositoryManager
// ==========================================

RepositoryManager::RepositoryManager(const std::string& base_dir)
    : base_dir_(base_dir) {
    if (!base_dir_.empty()) {
        std::error_code ec;
        fs::create_directories(base_dir_, ec);

        std::string conf_path = base_dir_ + "/repos.conf";
        if (fs::exists(conf_path)) {
            std::ifstream ifs(conf_path);
            std::string line;
            while (std::getline(ifs, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream iss(line);
                std::string name, type, url, enabled_str;
                if (std::getline(iss, name, '|') &&
                    std::getline(iss, type, '|') &&
                    std::getline(iss, url, '|')) {
                    std::getline(iss, enabled_str);
                    bool enabled = (enabled_str != "0" && enabled_str != "false" && enabled_str != "disabled");

                    if (type == "remote") {
                        add_repository(std::make_unique<RemoteRepository>(
                            name, url, base_dir_ + "/" + name, enabled));
                    } else {
                        add_repository(std::make_unique<LocalRepository>(
                            name, base_dir_ + "/" + name + "/repo.db", url, enabled));
                    }
                }
            }
        }

        // 若无仓库，则初始化默认 main 仓库
        if (repositories_.empty()) {
            add_repository(std::make_unique<LocalRepository>("main", base_dir_ + "/main/repo.db"));
            save_config();
        }
    }
}

void RepositoryManager::add_repository(std::unique_ptr<Repository> repo) {
    if (!repo) return;
    for (auto& r : repositories_) {
        if (r->name() == repo->name()) {
            r = std::move(repo);
            save_config();
            return;
        }
    }
    repositories_.push_back(std::move(repo));
    save_config();
}

void RepositoryManager::add(std::shared_ptr<Repository> repo) {
    if (!repo) return;
    for (auto& r : repositories_) {
        if (r->name() == repo->name()) {
            r = repo;
            save_config();
            return;
        }
    }
    repositories_.push_back(repo);
    save_config();
}

void RepositoryManager::remove_repository(const std::string& name) {
    repositories_.erase(
        std::remove_if(repositories_.begin(), repositories_.end(),
                       [&](const std::shared_ptr<Repository>& r) {
                           return r->name() == name;
                       }),
        repositories_.end()
    );
    save_config();
}

bool RepositoryManager::enable_repository(const std::string& name) {
    for (auto& r : repositories_) {
        if (r->name() == name) {
            r->set_enabled(true);
            save_config();
            return true;
        }
    }
    return false;
}

bool RepositoryManager::disable_repository(const std::string& name) {
    for (auto& r : repositories_) {
        if (r->name() == name) {
            r->set_enabled(false);
            save_config();
            return true;
        }
    }
    return false;
}

std::vector<Repository*> RepositoryManager::repositories() const {
    std::vector<Repository*> result;
    for (const auto& r : repositories_) {
        result.push_back(r.get());
    }
    return result;
}

std::vector<std::shared_ptr<Repository>> RepositoryManager::list() const {
    return repositories_;
}

Repository* RepositoryManager::get_repository(const std::string& name) const {
    for (const auto& r : repositories_) {
        if (r->name() == name) return r.get();
    }
    return nullptr;
}

std::optional<Object> RepositoryManager::find(const std::string& ref_or_ns, const std::string& name) const {
    if (!name.empty()) {
        for (const auto& r : repositories_) {
            if (!r->enabled()) continue;
            auto obj = r->find(ref_or_ns, name);
            if (obj) return obj;
        }
        return std::nullopt;
    }

    auto parsed = ObjectRef::parse(ref_or_ns);
    if (!parsed) return std::nullopt;
    return find_by_ref(*parsed);
}

std::optional<Object> RepositoryManager::find_by_ref(const ObjectRef& ref) const {
    for (const auto& r : repositories_) {
        if (!r->enabled()) continue;
        auto obj = r->find_by_ref(ref);
        if (obj) return obj;
    }
    return std::nullopt;
}

Collection<Object> RepositoryManager::search(const std::string& query) const {
    std::vector<Object> results;
    for (const auto& r : repositories_) {
        if (!r->enabled()) continue;
        auto col = r->search(query);
        for (const auto& obj : col.to_vector()) {
            results.push_back(obj);
        }
    }
    return Collection<Object>(std::move(results));
}

Collection<Object> RepositoryManager::find_pattern(const std::string& pattern) const {
    std::vector<Object> results;
    for (const auto& r : repositories_) {
        if (!r->enabled()) continue;
        auto col = r->find_pattern(pattern);
        for (const auto& obj : col.to_vector()) {
            results.push_back(obj);
        }
    }
    return Collection<Object>(std::move(results));
}

Collection<Object> RepositoryManager::list_all() const {
    std::vector<Object> results;
    for (const auto& r : repositories_) {
        if (!r->enabled()) continue;
        for (const auto& obj : r->list_objects()) {
            results.push_back(obj);
        }
    }
    return Collection<Object>(std::move(results));
}

bool RepositoryManager::sync_all() {
    bool ok = true;
    for (auto& r : repositories_) {
        if (r->enabled()) {
            if (!r->sync()) ok = false;
        }
    }
    return ok;
}

bool RepositoryManager::sync_repository(const std::string& name) {
    auto repo = get_repository(name);
    if (repo) {
        return repo->sync();
    }
    return false;
}

Repository* RepositoryManager::get_repository_for_object(const Object& obj) const {
    if (!obj.repository().empty()) {
        auto r = get_repository(obj.repository());
        if (r) return r;
    }
    for (const auto& r : repositories_) {
        auto found = r->find(obj.ns(), obj.name());
        if (found) return r.get();
    }
    return nullptr;
}

void RepositoryManager::save_config() const {
    if (base_dir_.empty()) return;
    std::string conf_path = base_dir_ + "/repos.conf";
    std::ofstream ofs(conf_path);
    if (!ofs.is_open()) return;

    ofs << "# Lunar Repository Configuration\n";
    for (const auto& r : repositories_) {
        ofs << r->name() << "|"
            << r->type_name() << "|"
            << r->url() << "|"
            << (r->enabled() ? "1" : "0") << "\n";
    }
}

} // namespace okrapm
