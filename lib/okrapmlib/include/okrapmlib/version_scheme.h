#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace okrapm {

// VersionScheme: 版本比较策略
//
// 目的：把"版本字符串怎么比大小"从 Object、ObjectRef、ArtifactMetadata 里抽出来。
// 现有的 Version 是 major.minor.patch 三个整数，表达不了 Fedora 的 EVR
// （例如 1:3.30.5-1.fc41）。方案见 backend.md 第 3 节。
//
// 本文件全部是新增代码，命名与缩进按 for_ai.md。同目录下 version.h 的旧风格
// （major_、minor_、is_pre_release 等）保持原样，不许重排重命名。

/**
 * enum VersionOrder - 三向比较结果。
 */
enum class VersionOrder
{
	Less = -1,
	Equal = 0,
	Greater = 1,
};

/**
 * class VersionScheme - 版本比较策略。
 */
class VersionScheme
{
public:
	virtual ~VersionScheme() = default;

	/**
	 * Name() - 返回策略名。
	 *
	 * Return: 稳定的短名，例如 "semver"、"rpm-evr"。这个名字会写进仓库配置，
	 *         发布之后不改。
	 */
	virtual std::string Name() const = 0;

	/**
	 * Compare() - 比较两个版本字符串。
	 * @Left: 左操作数，原样字符串，不预先解析。
	 * @Right: 右操作数，原样字符串，不预先解析。
	 *
	 * Return: Left 小于 Right 返回 VersionOrder::Less，相等返回 Equal，
	 *         大于返回 Greater。本函数不失败。
	 */
	virtual VersionOrder Compare(const std::string& Left, const std::string& Right) const = 0;

	/**
	 * IsValid() - 判断字符串是否属于本策略。
	 * @Raw: 待判定的版本字符串。
	 *
	 * Return: 属于返回 true，否则 false。
	 */
	virtual bool IsValid(const std::string& Raw) const = 0;
};

/**
 * struct RpmEvrParts - 拆好的 EVR 三段。
 * @Epoch: epoch，缺省 0。
 * @Version: version 段。
 * @Release: release 段。没有 '-' 时为空串。
 * @HasRelease: 是否出现了 '-'。
 *
 * HasRelease 单独留着，因为它在两处语义不同：
 *
 *   版本排序（RpmEvrCmp）  区分 "1-" 和 "1"。"1-" > "1"。
 *   依赖匹配（RpmMatchDependencyVersion）
 *                          把 "1-" 和 "1" 当同一件事，空 release 视为无。
 *
 * 所以本结构只负责拆，不负责归一化，由调用方决定。
 */
struct RpmEvrParts
{
	long long Epoch{0};
	std::string Version;
	std::string Release;
	bool HasRelease{false};
};

/**
 * RpmSplitEvr() - 把 [epoch:]version[-release] 拆成三段。
 * @Raw: 输入串。
 *
 * epoch 是第一个 ':' 之前的纯数字，不是纯数字时整串当 version。
 * release 是**最后一个** '-' 之后的部分。
 *
 * Return: 拆好的三段。本函数不失败。
 */
RpmEvrParts RpmSplitEvr(const std::string& Raw);

/**
 * RpmVerCmp() - 比较两个版本串，规则同 librpm 内部的 rpmvercmp()。
 * @Left: 左操作数。
 * @Right: 右操作数。
 *
 * 本函数只做"段比较"这一层：跳过非字母数字的分隔符，按数字段和字母段交替
 * 比较，处理 '~' 和 '^' 的特殊顺序，缺段按空串处理。它**不拆 epoch，也不拆
 * release**。
 *
 * 要比较完整的 EVR 用 RpmEvrCmp()。
 *
 * 注意：rpm 命令行的 rpm.vercmp（也就是 rpmdev-vercmp）会在调用这一层之前
 * 先按最后一个 '-' 拆出 release，所以它和本函数**不等价**。实测例子：
 *
 *   RpmVerCmp("1-3", "1-2-1")  >   纯段比较：段 3 对 2
 *   rpm.vercmp("1-3", "1-2-1") <   EVR 比较：version "1" 相同，release "3" 对 "2-1"
 *
 * 需要和 rpm 命令行对齐时，用 RpmEvrCmp()。
 *
 * Return: Left 小于 Right 返回负数，相等返回 0，大于返回正数。
 *         本函数不失败，不返回 errno。
 */
int RpmVerCmp(const std::string& Left, const std::string& Right);

/**
 * RpmEvrCmp() - 比较两个 RPM 的 EVR 串，语义与 rpm 命令行的 rpm.vercmp 一致。
 * @Left: 左操作数，形如 [epoch:]version[-release]。
 * @Right: 右操作数，同上。
 *
 * 顺序是 epoch、version、release。epoch 是第一个 ':' 之前的纯数字，缺省为 0。
 * release 是**最后一个** '-' 之后的部分，缺省为空串。每一段都用 RpmVerCmp()
 * 比。
 *
 * 本函数是拿来做版本排序的那一个。它的正确性用 tests/fuzz-cases.lua 生成的
 * 随机版本对和系统 rpm 做过差分测试。
 *
 * Return: Left 小于 Right 返回负数，相等返回 0，大于返回正数。
 *         本函数不失败，不返回 errno。
 */
int RpmEvrCmp(const std::string& Left, const std::string& Right);

/**
 * class RpmEvrScheme - RPM 的 EVR 版本策略。
 */
class RpmEvrScheme : public VersionScheme
{
public:
	/**
	 * Name() - 返回策略名。
	 *
	 * Return: 恒为 "rpm-evr"。
	 */
	std::string Name() const override;

	/**
	 * Compare() - 按 RPM 规则比较两个 EVR。
	 * @Left: 左操作数。
	 * @Right: 右操作数。
	 *
	 * Return: Left 小于 Right 返回 VersionOrder::Less，相等返回 Equal，大于返回 Greater。
	 */
	VersionOrder Compare(const std::string& Left, const std::string& Right) const override;

	/**
	 * IsValid() - 判断字符串是否可作为 RPM 版本。
	 * @Raw: 待判定的版本字符串。
	 *
	 * Return: 非空、且不含空格或制表符时返回 true。
	 */
	bool IsValid(const std::string& Raw) const override;
};

/**
 * class SemverScheme - 语义化版本策略，规则见 semver.org 2.0.0。
 */
class SemverScheme : public VersionScheme
{
public:
	/**
	 * Name() - 返回策略名。
	 *
	 * Return: 恒为 "semver"。
	 */
	std::string Name() const override;

	/**
	 * Compare() - 按语义化版本规则比较。
	 * @Left: 左操作数。
	 * @Right: 右操作数。
	 *
	 * Return: Left 小于 Right 返回 VersionOrder::Less，相等返回 Equal，大于返回 Greater。
	 */
	VersionOrder Compare(const std::string& Left, const std::string& Right) const override;

	/**
	 * IsValid() - 判断字符串是否是合法语义化版本。
	 * @Raw: 待判定的版本字符串。
	 *
	 * Return: 有 major、minor、patch 三段纯数字时返回 true。
	 */
	bool IsValid(const std::string& Raw) const override;
};

/**
 * class VersionSchemeRegistry - 策略注册表。
 */
class VersionSchemeRegistry
{
public:
	/**
	 * VersionSchemeRegistry() - 建立注册表，装入内建策略。
	 */
	VersionSchemeRegistry();

	/**
	 * Find() - 按名字取策略。
	 * @Scheme: 策略名，例如 "rpm-evr"。
	 *
	 * Return: 找到返回策略指针，所有权仍属注册表；找不到返回 nullptr。
	 */
	const VersionScheme* Find(const std::string& Scheme) const;

	/**
	 * Names() - 列出所有策略名。
	 *
	 * Return: 按注册顺序排列的名字列表。
	 */
	std::vector<std::string> Names() const;

	/**
	 * Compare() - 用指定策略比较两个版本。
	 * @Scheme: 策略名。
	 * @Left: 左操作数。
	 * @Right: 右操作数。
	 *
	 * Return: 成功时通过 OutOrder 返回结果并返回 0；策略名不认识时返回 -EINVAL。
	 */
	int Compare(const std::string& Scheme, const std::string& Left, const std::string& Right,
	            VersionOrder& OutOrder) const;

private:
	std::vector<std::unique_ptr<VersionScheme>> Schemes;
};

} // namespace okrapm