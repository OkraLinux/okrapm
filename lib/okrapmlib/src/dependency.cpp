#include "okrapmlib/dependency.h"

#include <cerrno>

namespace okrapm {

int ParseComparisonOperator(const std::string& Text, ComparisonOperator& OutOperator)
{
	if (Text.empty()) {
		OutOperator = ComparisonOperator::Any;
		return 0;
	}

	if (Text == "=" || Text == "==") {
		OutOperator = ComparisonOperator::Equal;
		return 0;
	}
	if (Text == ">") {
		OutOperator = ComparisonOperator::Greater;
		return 0;
	}
	if (Text == ">=") {
		OutOperator = ComparisonOperator::GreaterEqual;
		return 0;
	}
	if (Text == "<") {
		OutOperator = ComparisonOperator::Less;
		return 0;
	}
	if (Text == "<=") {
		OutOperator = ComparisonOperator::LessEqual;
		return 0;
	}

	return -EINVAL;
}

std::string ComparisonOperatorText(ComparisonOperator Operator)
{
	switch (Operator) {
	case ComparisonOperator::Any:
		return "";
	case ComparisonOperator::Less:
		return "<";
	case ComparisonOperator::LessEqual:
		return "<=";
	case ComparisonOperator::Equal:
		return "=";
	case ComparisonOperator::GreaterEqual:
		return ">=";
	case ComparisonOperator::Greater:
		return ">";
	}
	return "";
}

std::string DependencyKindText(DependencyKind Kind)
{
	switch (Kind) {
	case DependencyKind::Provides:
		return "provides";
	case DependencyKind::Requires:
		return "requires";
	case DependencyKind::Conflicts:
		return "conflicts";
	case DependencyKind::Obsoletes:
		return "obsoletes";
	case DependencyKind::Recommends:
		return "recommends";
	case DependencyKind::Suggests:
		return "suggests";
	case DependencyKind::Supplements:
		return "supplements";
	case DependencyKind::Enhances:
		return "enhances";
	}
	return "unknown";
}

int ParseDependencyKind(const std::string& Text, DependencyKind& OutKind)
{
	if (Text == "provides") {
		OutKind = DependencyKind::Provides;
		return 0;
	}
	if (Text == "requires") {
		OutKind = DependencyKind::Requires;
		return 0;
	}
	if (Text == "conflicts") {
		OutKind = DependencyKind::Conflicts;
		return 0;
	}
	if (Text == "obsoletes") {
		OutKind = DependencyKind::Obsoletes;
		return 0;
	}
	if (Text == "recommends") {
		OutKind = DependencyKind::Recommends;
		return 0;
	}
	if (Text == "suggests") {
		OutKind = DependencyKind::Suggests;
		return 0;
	}
	if (Text == "supplements") {
		OutKind = DependencyKind::Supplements;
		return 0;
	}
	if (Text == "enhances") {
		OutKind = DependencyKind::Enhances;
		return 0;
	}
	return -EINVAL;
}

std::string DependencySourceText(DependencySource Source)
{
	switch (Source) {
	case DependencySource::Unknown:
		return "unknown";
	case DependencySource::Manual:
		return "manual";
	case DependencySource::Automatic:
		return "auto";
	case DependencySource::Rpmlib:
		return "rpmlib";
	}
	return "unknown";
}

int ParseDependencySource(const std::string& Text, DependencySource& OutSource)
{
	// RPM 在 deptype 里写 "auto"，这里映射到 Automatic。
	if (Text == "manual") {
		OutSource = DependencySource::Manual;
		return 0;
	}
	if (Text == "auto" || Text == "automatic") {
		OutSource = DependencySource::Automatic;
		return 0;
	}
	if (Text == "rpmlib") {
		OutSource = DependencySource::Rpmlib;
		return 0;
	}
	if (Text == "unknown") {
		OutSource = DependencySource::Unknown;
		return 0;
	}
	return -EINVAL;
}

// StartsWithRpmlib() - 判断名字是否是 rpmlib(...) 形式。
// @Name: 能力名。
// Return: 是则返回 true。
static bool StartsWithRpmlib(const std::string& Name)
{
	static const std::string Prefix = "rpmlib(";
	if (Name.size() < Prefix.size()) {
		return false;
	}
	return Name.compare(0, Prefix.size(), Prefix) == 0;
}

bool DependencyIsInternal(const Dependency& Item)
{
	if (Item.Source == DependencySource::Rpmlib) {
		return true;
	}
	// 名字以 rpmlib( 开头的一律当内部能力。
	// 即使 deptype 没标成 rpmlib，这个名字也不可能是目标系统上的包名。
	return StartsWithRpmlib(Item.Name);
}

// RpmMatchDependencyVersion() - 依赖匹配语义下的版本比较。
// @CapabilityVersion: 提供方的版本。
// @RequirementVersion: 需求方的版本。
// @Operator: 比较符。
//
// Return: 满足返回 true。
//
// 这一套规则和版本排序（RpmEvrCmp）**不同**。实测自真实 rpm 求解，126 组
// 组合逐格核对过（见 tests/probe-full-matrix.sh 与 dep_matrix_full_main.cpp）：
//
//   1. epoch 不同            → 按 epoch 定胜负
//   2. version 段不同        → 按 version 定胜负
//   3. version 段相同，看 release：
//        a. 需求侧无 release   → 不约束 release，只比 version，视为相等
//        b. 需求侧有、提供侧无  → 提供侧通配，**恒满足**
//        c. 两侧都有           → 比 release
//
// **3a 和 3b 不对称**，这是最容易写错的地方。实测：
//
//   Provides: virt = 1.0    Requires: virt > 1.0-1   满足   （3b，提供侧通配）
//   Provides: virt = 1.0-1  Requires: virt > 1.0     失败   （3a，只比 version 后相等）
//
// 直觉上会写成对称的通配，那样会在 P1 x R7、P1 x R8、P2 x R7、P2 x R8 四格判错。
//
// 另外，空 release 在依赖匹配里视为"无"。所以 "1.0-" 和 "1.0" 在这里等价，
// 而在排序语义里 "1.0-" 比 "1.0" 大。
static bool RpmMatchDependencyVersion(const std::string& CapabilityVersion,
	                                  const std::string& RequirementVersion,
	                                  ComparisonOperator Operator)
{
	// 按比较符判定一个三向结果。
	// @Order: 负数表示提供方小，0 表示相等，正数表示提供方大。
	struct Local
	{
		static bool Judge(int Order, ComparisonOperator Which)
		{
			switch (Which) {
			case ComparisonOperator::Any:
				return true;
			case ComparisonOperator::Less:
				return Order < 0;
			case ComparisonOperator::LessEqual:
				return Order <= 0;
			case ComparisonOperator::Equal:
				return Order == 0;
			case ComparisonOperator::GreaterEqual:
				return Order >= 0;
			case ComparisonOperator::Greater:
				return Order > 0;
			}
			return false;
		}
	};

	const RpmEvrParts Capability = RpmSplitEvr(CapabilityVersion);
	const RpmEvrParts Requirement = RpmSplitEvr(RequirementVersion);

	if (Capability.Epoch != Requirement.Epoch) {
		return Local::Judge(Capability.Epoch < Requirement.Epoch ? -1 : 1, Operator);
	}

	const int VersionResult = RpmVerCmp(Capability.Version, Requirement.Version);
	if (VersionResult != 0) {
		return Local::Judge(VersionResult < 0 ? -1 : 1, Operator);
	}

	// 空 release 视为无。
	const bool CapabilityHasRelease = Capability.HasRelease && !Capability.Release.empty();
	const bool RequirementHasRelease = Requirement.HasRelease && !Requirement.Release.empty();

	if (!RequirementHasRelease) {
		// 3a：需求侧不约束 release，只比 version。到这里 version 已经相等。
		return Local::Judge(0, Operator);
	}
	if (!CapabilityHasRelease) {
		// 3b：提供侧没有 release，视为通配，满足任何比较符。
		return true;
	}

	// 3c：两侧都有 release。
	const int ReleaseResult = RpmVerCmp(Capability.Release, Requirement.Release);
	return Local::Judge(ReleaseResult < 0 ? -1 : (ReleaseResult > 0 ? 1 : 0), Operator);
}

int DependencySatisfiedBy(const Dependency& Requirement, const Dependency& Capability,
	                      const VersionScheme& Scheme, bool& OutSatisfied)
{
	OutSatisfied = false;

	const bool HasVersionConstraint = Requirement.Operator != ComparisonOperator::Any;
	if (HasVersionConstraint && Requirement.Version.empty()) {
		return -EINVAL;
	}

	// 名字必须逐字节相同。RPM 不做归一化。
	if (Requirement.Namespace != Capability.Namespace) {
		return 0;
	}
	if (Requirement.Name != Capability.Name) {
		return 0;
	}

	if (!HasVersionConstraint) {
		OutSatisfied = true;
		return 0;
	}

	// 能力没有版本时，视为"版本未指定"，满足任何约束。
	//
	// 这条是实测出来的，不是从直觉推的。用独立 rpmdb 跑了 12 组组合，rpm 的
	// 权威结果是：
	//
	//   Provides: virt      满足 Requires virt / virt >= 1.0 / virt >= 2.0 / virt = 1.5
	//   Provides: virt = 1.5  满足 virt / virt >= 1.0 / virt = 1.5，不满足 virt >= 2.0
	//   Provides: virt = 9.9  满足 virt / virt >= 1.0 / virt >= 2.0，不满足 virt = 1.5
	//
	// 第一行是关键：无版本的能力满足**一切**约束。
	//
	// 直觉上会写成"能力无版本不满足带版本的需求"，那是错的。按错的写，
	// Fedora 里所有用无版本 Provides 的虚拟能力（/usr/bin/sh、webserver 这类）
	// 都会被判成不能满足带版本的需求，解析器就会去找不存在的包。
	if (Capability.Version.empty()) {
		OutSatisfied = true;
		return 0;
	}

	// RPM 的依赖匹配走它自己那一套，不走 VersionScheme。
	// 原因是依赖匹配和版本排序对 release 的处理不同，见 RpmMatchDependencyVersion。
	// 如果 scheme 不是 rpm-evr，说明调用方混用了两套语义，这里退回到纯版本比较。
	if (Scheme.Name() == "rpm-evr") {
		OutSatisfied =
			RpmMatchDependencyVersion(Capability.Version, Requirement.Version, Requirement.Operator);
		return 0;
	}

	const VersionOrder Order = Scheme.Compare(Capability.Version, Requirement.Version);
	switch (Requirement.Operator) {
	case ComparisonOperator::Any:
		OutSatisfied = true;
		break;
	case ComparisonOperator::Less:
		OutSatisfied = Order == VersionOrder::Less;
		break;
	case ComparisonOperator::LessEqual:
		OutSatisfied = Order == VersionOrder::Less || Order == VersionOrder::Equal;
		break;
	case ComparisonOperator::Equal:
		OutSatisfied = Order == VersionOrder::Equal;
		break;
	case ComparisonOperator::GreaterEqual:
		OutSatisfied = Order == VersionOrder::Greater || Order == VersionOrder::Equal;
		break;
	case ComparisonOperator::Greater:
		OutSatisfied = Order == VersionOrder::Greater;
		break;
	}
	return 0;
}

std::vector<Dependency> FilterInternalDependencies(const std::vector<Dependency>& Items)
{
	std::vector<Dependency> Result;
	Result.reserve(Items.size());
	for (const Dependency& Item : Items) {
		if (DependencyIsInternal(Item)) {
			continue;
		}
		Result.push_back(Item);
	}
	return Result;
}

} // namespace okrapm