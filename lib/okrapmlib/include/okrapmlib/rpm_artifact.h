#pragma once

#include "okrapmlib/dependency.h"
#include "okrapmlib/rpm_repository.h"

#include <string>
#include <vector>

namespace okrapm {

// RpmArtifactBackend: RPM 包文件的后端
//
// 负责从 .rpm 文件提取元数据、文件列表、安装脚本。
// 对应 backend.md 第 6 节的 ArtifactBackend。
//
// 实现：调用 `rpm` 命令行（rpm -qp 系列），不自己解析 RPM 二进制格式。
// 原因：
//   1. RPM 二进制格式复杂（lead + signature header + main header + cpio payload），
//      自己写解析器容易出错，而且 rpm 工具已经做这件事了。
//   2. 这台机器有 rpm 4.18，稳定可靠。
//   3. 以后如果要去掉对 rpm 工具的依赖，再换成 librpm 的 C API 或自己解析。
//
// 用法：
//   RpmArtifactBackend Art;
//   auto Entry = Art.ReadMetadata("cmake-3.31.6-2.fc42.aarch64.rpm");
//   if (Entry) {
//     printf("%s-%s-%s\n", Entry->Name, Entry->Version, Entry->Release);
//   }

/**
 * struct RpmScriptlet - 包内安装脚本。
 */
struct RpmScriptlet
{
	std::string Phase;       // "pre"、"post"、"preun"、"postun"、"pretrans"、"posttrans"
	std::string Interpreter; // "/bin/sh" 等
	std::string Content;     // 脚本内容
};

/**
 * struct RpmPayloadEntry - payload 里的文件条目。
 */
struct RpmPayloadEntry
{
	std::string Path;
	std::string Mode;      // 例如 "0755"
	int64_t Size{0};
};

/**
 * class RpmArtifactBackend - RPM 包文件的后端。
 */
class RpmArtifactBackend
{
public:
	/**
	 * CanHandle() - 判断文件是否是 RPM。
	 * @LocalPath: 本地文件路径。
	 *
	 * Return: 是 RPM 返回 true。
	 */
	bool CanHandle(const std::string& LocalPath) const;

	/**
	 * ReadMetadata() - 读 RPM 的元数据。
	 * @LocalPath: .rpm 文件路径。
	 * @OutEntry: 输出解析结果。
	 *
	 * 提取：name、version、release、arch、summary、provides、requires、
	 * conflicts、obsoletes、recommends、suggests、size、location（设为 LocalPath）。
	 *
	 * Return: 成功返回 0，文件不存在返回 -ENOENT，rpm 命令失败返回 -EIO。
	 */
	int ReadMetadata(const std::string& LocalPath, RpmPackageEntry& OutEntry);

	/**
	 * ListPayload() - 列出 RPM 里的文件。
	 * @LocalPath: .rpm 文件路径。
	 * @OutEntries: 输出文件列表。
	 *
	 * Return: 成功返回 0。
	 */
	int ListPayload(const std::string& LocalPath,
	                std::vector<RpmPayloadEntry>& OutEntries);

	/**
	 * ListScriptlets() - 列出安装脚本。
	 * @LocalPath: .rpm 文件路径。
	 * @OutScriptlets: 输出脚本列表。
	 *
	 * Return: 成功返回 0。
	 */
	int ListScriptlets(const std::string& LocalPath,
	                   std::vector<RpmScriptlet>& OutScriptlets);

	/**
	 * ExtractPayload() - 解包到目标目录。
	 * @LocalPath: .rpm 文件路径。
	 * @InstallRoot: 目标根目录。
	 * @OutWritten: 输出实际写入的路径列表。
	 *
	 * Return: 成功返回 0。
	 */
	int ExtractPayload(const std::string& LocalPath, const std::string& InstallRoot,
	                   std::vector<std::string>& OutWritten);

private:
	/**
	 * RunCommand() - 执行命令，捕获 stdout。
	 * @Command: 完整命令行。
	 * @Output: 输出。
	 *
	 * Return: 退出码。0 表示成功。
	 */
	int RunCommand(const std::string& Command, std::string& Output);

	/**
	 * ParseDependencyList() - 把 `rpm -qp --requires` 的输出解析成 Dependency。
	 * @Output: rpm 命令的输出。
	 * @Kind: 依赖种类。
	 *
	 * 每行一个依赖，格式：
	 *   name
	 *   name = version-release
	 *   name >= version-release
	 *   (name = version if condition)   ← rich deps
	 */
	std::vector<Dependency> ParseDependencyList(const std::string& Output,
	                                             DependencyKind Kind);
};

} // namespace okrapm