#include "okrapmlib/rpm_resolver.h"

#include <algorithm>
#include <deque>
#include <functional>

namespace okrapm {

void RpmResolver::SetRepository(const RpmRepositoryBackend* Repo)
{
	Repository = Repo;
	ClearIndex();
}

void RpmResolver::SetInstalled(std::vector<RpmPackageEntry> Items)
{
	Installed = std::move(Items);
}

void RpmResolver::ClearIndex()
{
	ProvidesIndex.clear();
	IndexBuilt = false;
}

void RpmResolver::BuildIndex()
{
	ProvidesIndex.clear();
	if (Repository == nullptr) {
		IndexBuilt = true;
		return;
	}

	const auto& All = Repository->ListObjects();
	for (size_t Index = 0; Index < All.size(); Index++) {
		for (const auto& Prov : All[Index].Provides) {
			ProvidesIndex[Prov.Name].push_back(Index);
		}
	}
	IndexBuilt = true;
}

bool RpmResolver::PackageSatisfies(const RpmPackageEntry& Pkg, const Dependency& Need) const
{
	// 先看 Provides
	for (const auto& Prov : Pkg.Provides) {
		bool Satisfied = false;
		if (DependencySatisfiedBy(Need, Prov, Scheme, Satisfied) == 0 && Satisfied) {
			return true;
		}
	}

	// 再看包名本身。RPM 里包名也算一个隐式 Provides，但 primary.xml 里
	// 通常已经列出了 "name = ver-rel" 这条。这里兜个底，处理 name 精确匹配
	// 但 Provides 列表里没写的情况。
	if (Pkg.Name == Need.Name) {
		if (Need.Operator == ComparisonOperator::Any) {
			return true;
		}
		std::string PkgVersion = Pkg.Version;
		if (!Pkg.Release.empty()) {
			PkgVersion += "-" + Pkg.Release;
		}
		if (!Pkg.Epoch.empty() && Pkg.Epoch != "0") {
			PkgVersion = Pkg.Epoch + ":" + PkgVersion;
		}
		Dependency Capability;
		Capability.Name = Pkg.Name;
		Capability.Operator = ComparisonOperator::Equal;
		Capability.Version = PkgVersion;
		bool Satisfied = false;
		if (DependencySatisfiedBy(Need, Capability, Scheme, Satisfied) == 0 && Satisfied) {
			return true;
		}
	}

	return false;
}

bool RpmResolver::PoolSatisfies(const std::vector<RpmPackageEntry>& Pool,
                                const Dependency& Need) const
{
	for (const auto& Pkg : Pool) {
		if (PackageSatisfies(Pkg, Need)) {
			return true;
		}
	}
	return false;
}

bool RpmResolver::FindProviderIndex(const Dependency& Need, size_t& OutIndex) const
{
	auto It = ProvidesIndex.find(Need.Name);
	if (It == ProvidesIndex.end() || It->second.empty()) {
		return false;
	}

	const auto& All = Repository->ListObjects();

	// 优先选包名和需求名完全相同的
	for (size_t Candidate : It->second) {
		if (All[Candidate].Name == Need.Name) {
			OutIndex = Candidate;
			return true;
		}
	}

	// 否则选版本最高的
	size_t Best = It->second[0];
	for (size_t Candidate : It->second) {
		const auto& Left = All[Candidate];
		const auto& Right = All[Best];

		std::string LeftVersion = Left.Version;
		if (!Left.Release.empty()) {
			LeftVersion += "-" + Left.Release;
		}
		if (!Left.Epoch.empty() && Left.Epoch != "0") {
			LeftVersion = Left.Epoch + ":" + LeftVersion;
		}

		std::string RightVersion = Right.Version;
		if (!Right.Release.empty()) {
			RightVersion += "-" + Right.Release;
		}
		if (!Right.Epoch.empty() && Right.Epoch != "0") {
			RightVersion = Right.Epoch + ":" + RightVersion;
		}

		if (RpmEvrCmp(LeftVersion, RightVersion) > 0) {
			Best = Candidate;
		}
	}

	OutIndex = Best;
	return true;
}

std::vector<RpmPackageEntry> RpmResolver::TopologicalSort(
	const std::unordered_set<size_t>& Selected,
	const std::vector<RpmPackageEntry>& All) const
{
	// 递归 DFS 拓扑排序。
	// 对每个选中的包，先递归输出它的依赖（在 Selected 里的），
	// 然后输出自己。这样能保证被依赖的包在依赖方之前。

	std::vector<RpmPackageEntry> Result;
	std::unordered_set<size_t> Emitted;

	// 递归函数：用 lambda + 递归。
	std::function<void(size_t)> Visit = [&](size_t Index) {
		if (Emitted.count(Index) != 0) {
			return;
		}

		// 先标记，防止环导致的无限递归
		Emitted.insert(Index);

		// 先递归输出依赖（在 Selected 里的）
		for (const auto& Need : All[Index].Requires) {
			if (DependencyIsInternal(Need)) {
				continue;
			}
			// 跳过 rich deps
			if (!Need.Name.empty() && Need.Name[0] == '(') {
				continue;
			}
			// 找 Selected 里满足这条需求的包
			for (size_t Other : Selected) {
				if (Other == Index) {
					continue;
				}
				if (PackageSatisfies(All[Other], Need)) {
					Visit(Other);
					break;   // 一条需求只需要一个提供者
				}
			}
		}

		// 最后输出自己
		Result.push_back(All[Index]);
	};

	// 对所有选中的包做 DFS
	// 排序成有序列表，保证输出稳定
	std::vector<size_t> SortedSelected(Selected.begin(), Selected.end());
	std::sort(SortedSelected.begin(), SortedSelected.end());

	for (size_t Index : SortedSelected) {
		Visit(Index);
	}

	// 有环时（Emitted 在 Visit 递归里提前标记了），上面的顺序已经保证了
	// 每个包只出现一次。如果某些包没出现在 Result 里（不应该发生），
	// 补上。
	if (Result.size() < Selected.size()) {
		for (size_t Index : SortedSelected) {
			if (Emitted.count(Index) == 0) {
				Result.push_back(All[Index]);
				Emitted.insert(Index);
			}
		}
	}

	return Result;
}

RpmResolver::ResolveResult RpmResolver::ResolveInstall(const std::vector<std::string>& Requests)
{
	ResolveResult Result;
	Result.RequestedCount = Requests.size();

	if (Repository == nullptr) {
		Result.Errors.push_back("没有设置仓库");
		return Result;
	}

	if (!IndexBuilt) {
		BuildIndex();
	}

	const auto& All = Repository->ListObjects();

	// 选中的包（仓库下标）
	std::unordered_set<size_t> Selected;

	// 待处理的需求队列
	std::deque<Dependency> Pending;

	// 把请求变成需求
	for (const std::string& Name : Requests) {
		Dependency Need;
		Need.Name = Name;
		Need.Kind = DependencyKind::Requires;
		Need.Operator = ComparisonOperator::Any;
		Pending.push_back(Need);
	}

	// 已处理过的需求，避免重复
	std::unordered_set<std::string> SeenNeeds;

	// 防止无限循环
	size_t Iterations = 0;
	const size_t IterationLimit = All.size() * 4 + 1000;

	while (!Pending.empty()) {
		if (++Iterations > IterationLimit) {
			Result.Errors.push_back("依赖解析超过迭代上限，可能有环");
			break;
		}

		Dependency Need = Pending.front();
		Pending.pop_front();

		// 内部依赖跳过
		if (DependencyIsInternal(Need)) {
			continue;
		}

		// 去重
		std::string Key = Need.Name + "|" +
		                  ComparisonOperatorText(Need.Operator) + "|" + Need.Version;
		if (SeenNeeds.count(Key) != 0) {
			continue;
		}
		SeenNeeds.insert(Key);

		// 已被已安装的包满足
		if (PoolSatisfies(Installed, Need)) {
			continue;
		}

		// 已被选中的包满足
		bool SatisfiedBySelection = false;
		for (size_t Index : Selected) {
			if (PackageSatisfies(All[Index], Need)) {
				SatisfiedBySelection = true;
				break;
			}
		}
		if (SatisfiedBySelection) {
			continue;
		}

		// 找提供者
		size_t ProviderIndex = 0;
		if (!FindProviderIndex(Need, ProviderIndex)) {
			// 文件依赖和 soname 依赖找不到提供者时，不算致命错误——
			// 目标系统上可能已经有了（比如 libc.so.6 来自 glibc 包，
			// 但 glibc 在 Fedora 仓库里的 Provides 可能不完整）。
			if (Need.Name[0] == '/' ||
			    Need.Name.find("()") != std::string::npos) {
				Result.Errors.push_back(
					"找不到提供者（文件或 soname，目标系统需已有）: " + Need.Name);
				continue;
			}
			Result.Errors.push_back("找不到提供者: " + Need.Name);
			continue;
		}

		// 选中它
		Selected.insert(ProviderIndex);

		// 把它的 requires 加进队列
		for (const auto& SubNeed : All[ProviderIndex].Requires) {
			Pending.push_back(SubNeed);
		}
	}

	// 拓扑排序
	Result.Plan = TopologicalSort(Selected, All);
	Result.NewPackages = Result.Plan.size();

	// Success 的判定：所有非文件/非 soname 的需求都有解
	bool HasFatal = false;
	for (const auto& Error : Result.Errors) {
		if (Error.find("找不到提供者（文件或 soname") == std::string::npos) {
			HasFatal = true;
			break;
		}
	}
	Result.Success = !HasFatal;

	return Result;
}

} // namespace okrapm