#pragma once

#include "okrapmlib/dependency.h"
#include "okrapmlib/version_scheme.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace okrapm {

// RpmRepositoryBackend: RPM 仓库后端
//
// 负责读 repomd.xml 和 primary.xml，解析成 Lunar 的 Object + Dependency。
//
// 设计目标（见 backend.md 第 4 节）：
//   - sync()：下载或更新索引
//   - list_objects()：列出所有包
//   - find()：按命名空间+名字查找
//   - search()：按关键字搜索
//
// 这一版只做只读原型：读本地 primary.xml，不做网络下载。
// 网络下载等 RepositoryManager 的下载器就位后再接。

/**
 * struct RpmPackageEntry - 从 primary.xml 解析出的一个包。
 */
struct RpmPackageEntry
{
	std::string Name;
	std::string Arch;
	std::string Epoch;     // 空串表示 0
	std::string Version;
	std::string Release;
	std::string Summary;
	std::string Location;   // Packages/x/xxx.aarch64.rpm
	int64_t PackageSize{0};
	int64_t InstalledSize{0};
	std::string Sha256;

	std::vector<Dependency> Provides;
	std::vector<Dependency> Requires;
	std::vector<Dependency> Conflicts;
	std::vector<Dependency> Obsoletes;
	std::vector<Dependency> Recommends;
	std::vector<Dependency> Suggests;
};

/**
 * struct RpmRepoMetadata - 从 repomd.xml 解析出的元数据引用。
 */
struct RpmRepoMetadataRef
{
	std::string DataType;    // "primary"、"filelists"、"other"、"group"
	std::string Location;    // repodata/xxx-primary.xml.zst
	std::string Checksum;
	std::string ChecksumType;
	int64_t Size{0};
	int64_t OpenSize{0};
};

/**
 * class RpmRepositoryBackend - RPM 仓库后端。
 *
 * 用法：
 *   RpmRepositoryBackend Repo;
 *   Repo.SetBaseUrl("https://mirrors.aliyun.com/fedora/releases/42/Everything/aarch64/os");
 *   Repo.LoadFromLocal("/path/to/primary.xml");
 *   Repo.ListObjects();  // 返回所有包
 *   Repo.Find("fedora", "cmake");  // 按命名空间+名字查
 */
class RpmRepositoryBackend
{
public:
	/**
	 * SetBaseUrl() - 设置仓库的 base URL。
	 * @Url: 仓库根 URL，例如 https://mirrors.aliyun.com/fedora/releases/42/Everything/aarch64/os
	 *
	 * 用于拼接 location 里的相对路径（Packages/x/xxx.rpm）。
	 */
	void SetBaseUrl(const std::string& Url);

	/**
	 * SetNamespace() - 设置包的命名空间。
	 * @Ns: 命名空间名，例如 "fedora"。
	 *
	 * 所有从该仓库解析出的包都会带上这个命名空间。
	 */
	void SetNamespace(const std::string& Ns);

	/**
	 * LoadRepomd() - 从本地文件或文本加载 repomd.xml。
	 * @Xml: repomd.xml 的完整文本。
	 *
	 * Return: 成功返回 0，解析失败返回 -EINVAL。
	 */
	int LoadRepomd(const std::string& Xml);

	/**
	 * LoadPrimary() - 从本地文件加载 primary.xml。
	 * @Xml: primary.xml 的完整文本。
	 *
	 * Return: 成功返回 0，解析失败返回 -EINVAL。
	 */
	int LoadPrimary(const std::string& Xml);

	/**
	 * LoadPrimaryFromFile() - 从文件加载 primary.xml。
	 * @Path: primary.xml 的本地路径。
	 *
	 * Return: 成功返回 0，文件打不开返回 -ENOENT，解析失败返回 -EINVAL。
	 */
	int LoadPrimaryFromFile(const std::string& Path);

	/**
	 * ListObjects() - 列出所有已加载的包。
	 *
	 * Return: 包列表的 const 引用。
	 */
	const std::vector<RpmPackageEntry>& ListObjects() const;

	/**
	 * Find() - 按名字查找包。
	 * @Name: 包名。
	 *
	 * Return: 找到返回指针，找不到返回 nullptr。如果有多版本，返回最新版。
	 */
	const RpmPackageEntry* Find(const std::string& Name) const;

	/**
	 * FindAll() - 按名字查找所有版本的包。
	 * @Name: 包名。
	 *
	 * Return: 匹配的包列表。
	 */
	std::vector<const RpmPackageEntry*> FindAll(const std::string& Name) const;

	/**
	 * Search() - 按关键字搜索包名。
	 * @Query: 搜索关键字。
	 *
	 * Return: 匹配的包列表（名字包含关键字的）。
	 */
	std::vector<const RpmPackageEntry*> Search(const std::string& Query) const;

	/**
	 * WhatProvides() - 查找提供指定能力的包。
	 * @CapabilityName: 能力名，可以是包名、文件路径、soname。
	 *
	 * Return: 所有提供该能力的包。
	 */
	std::vector<const RpmPackageEntry*> WhatProvides(const std::string& CapabilityName) const;

	/**
	 * PackageCount() - 返回已加载的包数量。
	 *
	 * Return: 包数量。
	 */
	size_t PackageCount() const;

	/**
	 * GetNamespace() - 返回命名空间。
	 *
	 * Return: 命名空间名。
	 */
	const std::string& GetNamespace() const;

private:
	std::string BaseUrl;
	std::string Namespace{"fedora"};
	std::vector<RpmRepoMetadataRef> MetadataRefs;
	std::vector<RpmPackageEntry> Packages;

	// 解析 repomd.xml 里的 <data> 条目。
	void ParseRepomdData(const std::string& Xml);

	// 解析 primary.xml 里的 <package> 条目。
	void ParsePrimaryPackage(const std::string& Xml);

	// 解析 rpm:requires / rpm:provides 等 <rpm:entry> 列表。
	std::vector<Dependency> ParseDependencyEntries(const std::string& EntryXml,
	                                                DependencyKind Kind);

	// 从 rpm:entry 的属性构造 Dependency。
	Dependency ParseEntry(const std::string& Name, const std::string& Flags,
	                      const std::string& Epoch, const std::string& Ver,
	                      const std::string& Rel, DependencyKind Kind);
};

} // namespace okrapm