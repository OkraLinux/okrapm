#pragma once

#include "okrapmlib/dependency.h"
#include "okrapmlib/rpm_repository.h"
#include "okrapmlib/version_scheme.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace okrapm {

// RpmResolver: RPM 依赖解析器
//
// 对应 backend.md 第 5 节的 Resolver。把"要装什么"变成"要装哪些包、按什么顺序"。
//
// 算法：贪心闭包 + 拓扑排序。
//
//   1. 把请求当成初始需求放进工作队列。
//   2. 逐条处理需求：
//        a. rpmlib(...) 这类内部依赖跳过。
//        b. 已被已安装的包满足 → 跳过。
//        c. 已被本次选中的包满足 → 跳过。
//        d. 否则在 Provides 索引里找提供者，选中它，把它的 requires 加进队列。
//        e. 找不到提供者 → 记一条错误。
//   3. 对选中的包做拓扑排序，依赖在前。
//
// **这不是 SAT 求解器。** 它保证找到的解是**有效的**（闭包成立、顺序正确），
// 但不保证是最小的，也不保证和 dnf 的选择完全一致。Fedora 的依赖求解用 libsolv，
// 处理备选方案和冲突比贪心复杂。要做成那样，应该把 libsolv 接进来（见 backend.md
// 第 5 节），而不是自己写 SAT。
//
// 用法：
//   RpmResolver Resolver;
//   Resolver.SetRepository(&Repo);
//   auto Result = Resolver.ResolveInstall({"cmake"});
//   if (Result.Success) {
//     for (const auto& Pkg : Result.Plan) { ... }   // 按安装顺序
//   }

/**
 * class RpmResolver - RPM 依赖解析器。
 */
class RpmResolver
{
public:
	/**
	 * struct ResolveResult - 解析结果。
	 * @Success: 全部需求都解出来了。
	 * @Plan: 要安装的包，按依赖顺序排列，被依赖的在前。
	 * @Errors: 解不出来的需求，每条一句说明。
	 * @RequestedCount: 用户请求的包数。
	 * @NewPackages: 计划里要装的包数（等于 Plan.size()）。
	 */
	struct ResolveResult
	{
		bool Success{false};
		std::vector<RpmPackageEntry> Plan;
		std::vector<std::string> Errors;
		size_t RequestedCount{0};
		size_t NewPackages{0};
	};

	/**
	 * SetRepository() - 设置仓库。
	 * @Repo: 已加载的仓库。生命周期由调用方保证。
	 */
	void SetRepository(const RpmRepositoryBackend* Repo);

	/**
	 * SetInstalled() - 设置已安装的包。
	 * @Items: 已安装包列表。这些包的需求不再解析。
	 */
	void SetInstalled(std::vector<RpmPackageEntry> Items);

	/**
	 * ResolveInstall() - 解析安装请求。
	 * @Requests: 要安装的包名列表。
	 *
	 * Return: 解析结果。Result.Success 为 false 时看 Result.Errors。
	 */
	ResolveResult ResolveInstall(const std::vector<std::string>& Requests);

	/**
	 * ClearIndex() - 清掉 Provides 索引。
	 *
	 * 换了仓库之后要调用，否则会用旧索引。
	 */
	void ClearIndex();

private:
	const RpmRepositoryBackend* Repository{nullptr};
	std::vector<RpmPackageEntry> Installed;
	bool IndexBuilt{false};
	std::unordered_map<std::string, std::vector<size_t>> ProvidesIndex;
	RpmEvrScheme Scheme;

	// 建 Provides 索引：能力名 → 提供它的包的下标列表。
	void BuildIndex();

	// 判断一个包是否满足一条需求。
	bool PackageSatisfies(const RpmPackageEntry& Pkg, const Dependency& Need) const;

	// 判断一组包里是否有包满足这条需求。
	bool PoolSatisfies(const std::vector<RpmPackageEntry>& Pool,
	                   const Dependency& Need) const;

	// 找最合适的提供者，返回它在仓库里的下标。
	// 选择顺序：包名和需求名完全相同的优先，然后版本高的优先。
	bool FindProviderIndex(const Dependency& Need, size_t& OutIndex) const;

	// 对选中的包做拓扑排序。
	std::vector<RpmPackageEntry> TopologicalSort(
		const std::unordered_set<size_t>& Selected,
		const std::vector<RpmPackageEntry>& All) const;
};

} // namespace okrapm