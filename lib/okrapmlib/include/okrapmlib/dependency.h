#pragma once

#include "okrapmlib/version_scheme.h"

#include <string>
#include <vector>

namespace okrapm {

// Dependency: 带约束的依赖与能力
//
// 背景：object.h 里现有的 dependencies_ 是 std::vector<std::string>，只能表达
// "GNU.make" 这种裸包名。RPM 需要的是：
//
//   Requires:    glibc >= 2.39
//   Requires:    /usr/bin/sh
//   Requires:    libstdc++.so.6()(64bit)
//   Provides:    cmake = 3.30.5-1.fc41
//   Conflicts:   cmake < 3.28
//   Obsoletes:   cmake3 < 3.30
//
// 其中依赖名可以是包名、文件路径、soname、或者 rpm 的虚拟能力名，都当字符串比。
//
// 本文件全部是新增代码，命名与缩进按 for_ai.md。object.h 里的 dependencies_
// 保持原样，两条路并存。

/**
 * enum DependencyKind - 依赖种类。
 *
 * 取值与 RPM 的标签集一一对应，实测自 rpm 4.18 的 --querytags：
 * PROVIDES / REQUIRES / CONFLICTS / OBSOLETES / RECOMMENDS / SUGGESTS /
 * SUPPLEMENTS / ENHANCES。
 */
enum class DependencyKind
{
	Provides,
	Requires,
	Conflicts,
	Obsoletes,
	Recommends,
	Suggests,
	Supplements,
	Enhances,
};

/**
 * enum DependencySource - 依赖的来源。
 *
 * 取自 RPM 的 deptype。实测 rpm 4.18 上出现过的取值只有 manual、auto、
 * rpmlib 三个：
 *
 *   manual   spec 里手写的
 *   auto     打包器自动生成的，例如从 ELF 的 soname 推出 libc.so.6()(64bit)
 *   rpmlib   rpmlib(...) 形式的内部能力，描述的是 RPM 自己的特性
 *
 * rpmlib 那类必须能被过滤掉，否则解析器会去找根本不存在的"包"。
 */
enum class DependencySource
{
	Unknown,
	Manual,
	Automatic,
	Rpmlib,
};

/**
 * enum ComparisonOperator - 版本比较符。
 */
enum class ComparisonOperator
{
	Any,          // 无约束
	Less,
	LessEqual,
	Equal,
	GreaterEqual,
	Greater,
};

/**
 * struct Dependency - 一条依赖或一条能力。
 * @Namespace: 命名空间。RPM 的能力名没有命名空间，留空。
 * @Name: 名字。包名、文件路径、soname、虚拟能力名，都按原样存。
 * @Operator: 版本比较符。
 * @Version: 版本字符串，按所属 scheme 解释。
 * @Kind: 依赖种类。
 * @Source: 来源。
 */
struct Dependency
{
	std::string Namespace;
	std::string Name;
	ComparisonOperator Operator{ComparisonOperator::Any};
	std::string Version;
	DependencyKind Kind{DependencyKind::Requires};
	DependencySource Source{DependencySource::Unknown};
};

/**
 * ParseComparisonOperator() - 把符号串转成枚举。
 * @Text: 符号串，可以是 ">"、">="、"<"、"<="、"=="、"= "，或者空串。
 *
 * Return: 成功返回 0 并通过 OutOperator 返回结果。无法识别时返回 -EINVAL。
 */
int ParseComparisonOperator(const std::string& Text, ComparisonOperator& OutOperator);

/**
 * ComparisonOperatorText() - 把枚举转回符号串。
 * @Operator: 比较符。
 *
 * Return: 符号串。"=" 用 "=" 表示，无约束返回空串。
 */
std::string ComparisonOperatorText(ComparisonOperator Operator);

/**
 * DependencyKindText() - 依赖种类的英文名。
 * @Kind: 依赖种类。
 *
 * Return: 小写英文名，例如 "requires"。
 */
std::string DependencyKindText(DependencyKind Kind);

/**
 * ParseDependencyKind() - 把英文名转成依赖种类。
 * @Text: 小写英文名。
 *
 * Return: 成功返回 0 并通过 OutKind 返回结果。无法识别时返回 -EINVAL。
 */
int ParseDependencyKind(const std::string& Text, DependencyKind& OutKind);

/**
 * DependencySourceText() - 来源的英文名。
 * @Source: 来源。
 *
 * Return: 小写英文名，例如 "manual"。未知返回 "unknown"。
 */
std::string DependencySourceText(DependencySource Source);

/**
 * ParseDependencySource() - 把英文名转成来源。
 * @Text: 小写英文名，取自 RPM 的 deptype。
 *
 * Return: 成功返回 0 并通过 OutSource 返回结果。无法识别时返回 -EINVAL。
 *         RPM 的 "auto" 映射到 Automatic。
 */
int ParseDependencySource(const std::string& Text, DependencySource& OutSource);

/**
 * DependencyIsInternal() - 判断一条依赖是否描述 RPM 自身而非目标系统。
 * @Item: 待判断的依赖。
 *
 * Return: 是内部依赖返回 true。这类依赖不应进入解析器的候选集。
 */
bool DependencyIsInternal(const Dependency& Item);

/**
 * DependencySatisfiedBy() - 判断一条能力是否满足一条依赖。
 * @Requirement: 需求方，通常是 Requires。
 * @Capability: 供给方，通常是某个对象的 Provides。
 * @Scheme: 比较版本用的策略。
 * @OutSatisfied: 输出是否满足。
 *
 * 判定顺序：
 *   1. Namespace 与 Name 必须逐字节相同。RPM 不做任何归一化，文件路径和
 *      soname 都只是普通名字。
 *   2. Requirement 的 Operator 是 Any 时，名字对上就算满足。
 *   3. Capability 没有版本时，视为"版本未指定"，满足任何约束。
 *   4. 否则按 Scheme 比较两个版本，再按 Operator 判定。
 *
 * 第 3 条是实测出来的，不是从直觉推的。用独立 rpmdb 跑了 12 组组合：
 *
 *   Provides: virt        满足 Requires virt / virt >= 1.0 / virt >= 2.0 / virt = 1.5
 *   Provides: virt = 1.5  满足 virt / virt >= 1.0 / virt = 1.5，不满足 virt >= 2.0
 *   Provides: virt = 9.9  满足 virt / virt >= 1.0 / virt >= 2.0，不满足 virt = 1.5
 *
 * 直觉上容易写成"能力无版本不满足带版本的需求"，那是错的。按错的写，Fedora
 * 里所有用无版本 Provides 的虚拟能力都会被判成不可满足。
 *
 * Return: 成功返回 0，参数不合法返回 -EINVAL。参数不合法指的是：Operator 是
 *         版本比较符但 Version 为空。名字不匹配是正常的"不满足"，返回 0 并把
 *         OutSatisfied 置 false。
 */
int DependencySatisfiedBy(const Dependency& Requirement, const Dependency& Capability,
	const VersionScheme& Scheme, bool& OutSatisfied);

/**
 * FilterInternalDependencies() - 去掉内部依赖。
 * @Items: 输入列表。
 *
 * Return: 过滤后的列表，保持原有顺序。
 */
std::vector<Dependency> FilterInternalDependencies(const std::vector<Dependency>& Items);

} // namespace okrapm