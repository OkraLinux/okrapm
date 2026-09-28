#include "okrapmlib/version_scheme.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace okrapm {

// 下面三个判断只看 ASCII，不用 isalpha / isdigit。
// 那两个函数受 locale 影响，librpm 自己用的也是 ASCII 版本。

static bool IsAsciiDigit(char Character)
{
	return Character >= '0' && Character <= '9';
}

static bool IsAsciiAlpha(char Character)
{
	return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z');
}

static bool IsAsciiAlnum(char Character)
{
	return IsAsciiDigit(Character) || IsAsciiAlpha(Character);
}

static bool IsAllDigits(const std::string& Text)
{
	if (Text.empty()) {
		return false;
	}
	for (char Character : Text) {
		if (!IsAsciiDigit(Character)) {
			return false;
		}
	}
	return true;
}

static std::vector<std::string> SplitBy(const std::string& Text, char Separator)
{
	std::vector<std::string> Parts;
	std::string Current;
	for (char Character : Text) {
		if (Character == Separator) {
			Parts.push_back(Current);
			Current.clear();
		} else {
			Current.push_back(Character);
		}
	}
	Parts.push_back(Current);
	return Parts;
}

int RpmVerCmp(const std::string& Left, const std::string& Right)
{
	// 逐字节相同就不用进循环。librpm 也是先做这一步。
	if (Left == Right) {
		return 0;
	}

	// 复制一份再改。段尾要临时写成 '\0' 才能用 strcmp，所以不能直接改入参。
	std::string LeftBuffer = Left;
	std::string RightBuffer = Right;
	LeftBuffer.push_back('\0');
	RightBuffer.push_back('\0');

	char* One = LeftBuffer.data();
	char* Two = RightBuffer.data();
	bool IsNumeric = false;

	while (*One != '\0' || *Two != '\0') {
		// 跳过分隔符。波浪号和脱字符要留下来，它们参与比较。
		while (*One != '\0' && !IsAsciiAlnum(*One) && *One != '~' && *One != '^') {
			One++;
		}
		while (*Two != '\0' && !IsAsciiAlnum(*Two) && *Two != '~' && *Two != '^') {
			Two++;
		}

		// 波浪号排在任何东西之前。
		if (*One == '~' || *Two == '~') {
			if (*One != '~') {
				return 1;
			}
			if (*Two != '~') {
				return -1;
			}
			One++;
			Two++;
			continue;
		}

		// 脱字符排在任何东西之后。一方已经到基础版本时，另一方更大。
		if (*One == '^' || *Two == '^') {
			if (*One == '\0') {
				return -1;
			}
			if (*Two == '\0') {
				return 1;
			}
			if (*One != '^') {
				return 1;
			}
			if (*Two != '^') {
				return -1;
			}
			One++;
			Two++;
			continue;
		}

		// 有一边结束了，跳出后按剩余长度定胜负。
		if (*One == '\0' || *Two == '\0') {
			break;
		}

		char* SegmentOne = One;
		char* SegmentTwo = Two;

		// 段类型由左边第一个字符决定，右边按同一类型切。
		if (IsAsciiDigit(*SegmentOne)) {
			while (*SegmentOne != '\0' && IsAsciiDigit(*SegmentOne)) {
				SegmentOne++;
			}
			while (*SegmentTwo != '\0' && IsAsciiDigit(*SegmentTwo)) {
				SegmentTwo++;
			}
			IsNumeric = true;
		} else {
			while (*SegmentOne != '\0' && IsAsciiAlpha(*SegmentOne)) {
				SegmentOne++;
			}
			while (*SegmentTwo != '\0' && IsAsciiAlpha(*SegmentTwo)) {
				SegmentTwo++;
			}
			IsNumeric = false;
		}

		const char LeftTail = *SegmentOne;
		*SegmentOne = '\0';
		const char RightTail = *SegmentTwo;
		*SegmentTwo = '\0';

		// 左边的段不可能为空：进循环前已保证 One 指向字母或数字。
		if (One == SegmentOne) {
			return -1;
		}

		// 右边的段为空，说明两边段类型不同。数字段比字母段新。
		if (Two == SegmentTwo) {
			return IsNumeric ? 1 : -1;
		}

		if (IsNumeric) {
			// 去掉前导零，然后按位数比，避免长数字段溢出。
			while (*One == '0') {
				One++;
			}
			while (*Two == '0') {
				Two++;
			}

			const size_t LeftLength = std::strlen(One);
			const size_t RightLength = std::strlen(Two);
			if (LeftLength != RightLength) {
				return LeftLength > RightLength ? 1 : -1;
			}
		}

		const int Result = std::strcmp(One, Two);
		if (Result != 0) {
			return Result < 0 ? -1 : 1;
		}

		// 这一段相同，恢复段尾，从下一个字符继续。
		*SegmentOne = LeftTail;
		One = SegmentOne;
		*SegmentTwo = RightTail;
		Two = SegmentTwo;
	}

	if (*One == '\0' && *Two == '\0') {
		return 0;
	}
	return *One == '\0' ? -1 : 1;
}

// SplitEpoch() - 把 [epoch:]rest 拆成 epoch 和 rest。
// @Raw: 输入串。
// @OutEpoch: 输出 epoch，缺省为 0。
// @OutRest: 输出余下部分。
static void SplitEpoch(const std::string& Raw, long long& OutEpoch, std::string& OutRest)
{
	const size_t Colon = Raw.find(':');
	if (Colon == std::string::npos) {
		OutEpoch = 0;
		OutRest = Raw;
		return;
	}

	const std::string Head = Raw.substr(0, Colon);
	if (!IsAllDigits(Head)) {
		// 冒号左边不是纯数字，整个串当 version，不认 epoch。
		OutEpoch = 0;
		OutRest = Raw;
		return;
	}

	OutEpoch = std::strtoll(Head.c_str(), nullptr, 10);
	OutRest = Raw.substr(Colon + 1);
}

RpmEvrParts RpmSplitEvr(const std::string& Raw)
{
	RpmEvrParts Parts;

	std::string Rest;
	SplitEpoch(Raw, Parts.Epoch, Rest);

	const size_t Dash = Rest.rfind('-');
	if (Dash == std::string::npos) {
		Parts.Version = Rest;
		Parts.Release.clear();
		Parts.HasRelease = false;
		return Parts;
	}

	Parts.Version = Rest.substr(0, Dash);
	Parts.Release = Rest.substr(Dash + 1);
	Parts.HasRelease = true;
	return Parts;
}

int RpmEvrCmp(const std::string& Left, const std::string& Right)
{
	const RpmEvrParts LeftParts = RpmSplitEvr(Left);
	const RpmEvrParts RightParts = RpmSplitEvr(Right);

	if (LeftParts.Epoch != RightParts.Epoch) {
		return LeftParts.Epoch < RightParts.Epoch ? -1 : 1;
	}

	const int VersionResult = RpmVerCmp(LeftParts.Version, RightParts.Version);
	if (VersionResult != 0) {
		return VersionResult < 0 ? -1 : 1;
	}

	// release 的有无先定胜负，然后才比内容。
	// 注意这里用的是 RpmEvrParts::HasRelease，不是 Release 是否为空。
	// 排序语义下 "1-" 比 "1" 大，"1-" 和 "1-." 相等。
	if (LeftParts.HasRelease != RightParts.HasRelease) {
		return LeftParts.HasRelease ? 1 : -1;
	}
	if (!LeftParts.HasRelease) {
		return 0;
	}

	const int ReleaseResult = RpmVerCmp(LeftParts.Release, RightParts.Release);
	if (ReleaseResult != 0) {
		return ReleaseResult < 0 ? -1 : 1;
	}
	return 0;
}

std::string RpmEvrScheme::Name() const
{
	return "rpm-evr";
}

VersionOrder RpmEvrScheme::Compare(const std::string& Left, const std::string& Right) const
{
	const int Result = RpmEvrCmp(Left, Right);
	if (Result < 0) {
		return VersionOrder::Less;
	}
	if (Result > 0) {
		return VersionOrder::Greater;
	}
	return VersionOrder::Equal;
}

bool RpmEvrScheme::IsValid(const std::string& Raw) const
{
	if (Raw.empty()) {
		return false;
	}
	for (char Character : Raw) {
		if (Character == ' ' || Character == '\t') {
			return false;
		}
	}
	return true;
}

// SemverParts - 拆好的语义化版本。
struct SemverParts
{
	bool Valid{false};
	long long Major{0};
	long long Minor{0};
	long long Patch{0};
	std::vector<std::string> PreRelease;
};

// ParseSemver() - 拆一个语义化版本串。
// @Raw: 输入串，形如 major.minor.patch[-pre][+build]。
// Return: 拆好的结果。Valid 为 false 表示串不合法。
static SemverParts ParseSemver(const std::string& Raw)
{
	SemverParts Result;
	std::string Body = Raw;

	// 构建元数据不参与比较，先切掉。
	const size_t Plus = Body.find('+');
	if (Plus != std::string::npos) {
		Body = Body.substr(0, Plus);
	}

	const size_t Dash = Body.find('-');
	if (Dash != std::string::npos) {
		const std::string PreRelease = Body.substr(Dash + 1);
		Body = Body.substr(0, Dash);
		if (!PreRelease.empty()) {
			Result.PreRelease = SplitBy(PreRelease, '.');
		}
	}

	const std::vector<std::string> Numbers = SplitBy(Body, '.');
	if (Numbers.size() != 3) {
		return Result;
	}
	if (!IsAllDigits(Numbers[0]) || !IsAllDigits(Numbers[1]) || !IsAllDigits(Numbers[2])) {
		return Result;
	}

	Result.Major = std::strtoll(Numbers[0].c_str(), nullptr, 10);
	Result.Minor = std::strtoll(Numbers[1].c_str(), nullptr, 10);
	Result.Patch = std::strtoll(Numbers[2].c_str(), nullptr, 10);
	Result.Valid = true;
	return Result;
}

// ComparePreRelease() - 比较两串预发布标识符。
// @Left: 左边的标识符列表。
// @Right: 右边的标识符列表。
// Return: 负数表示 Left 小，0 表示相等，正数表示 Left 大。
static int ComparePreRelease(const std::vector<std::string>& Left,
	                         const std::vector<std::string>& Right)
{
	// 有预发布标识符的版本比没有的小。
	if (Left.empty() || Right.empty()) {
		if (Left.empty() && Right.empty()) {
			return 0;
		}
		return Left.empty() ? 1 : -1;
	}

	const size_t Common = std::min(Left.size(), Right.size());
	for (size_t Index = 0; Index < Common; Index++) {
		const std::string& LeftPart = Left[Index];
		const std::string& RightPart = Right[Index];
		const bool LeftNumeric = IsAllDigits(LeftPart);
		const bool RightNumeric = IsAllDigits(RightPart);

		if (LeftNumeric && RightNumeric) {
			const long long LeftValue = std::strtoll(LeftPart.c_str(), nullptr, 10);
			const long long RightValue = std::strtoll(RightPart.c_str(), nullptr, 10);
			if (LeftValue != RightValue) {
				return LeftValue < RightValue ? -1 : 1;
			}
			continue;
		}

		// 纯数字标识符比带字母的小。
		if (LeftNumeric != RightNumeric) {
			return LeftNumeric ? -1 : 1;
		}

		const int Result = LeftPart.compare(RightPart);
		if (Result != 0) {
			return Result < 0 ? -1 : 1;
		}
	}

	if (Left.size() == Right.size()) {
		return 0;
	}
	return Left.size() < Right.size() ? -1 : 1;
}

std::string SemverScheme::Name() const
{
	return "semver";
}

bool SemverScheme::IsValid(const std::string& Raw) const
{
	return ParseSemver(Raw).Valid;
}

VersionOrder SemverScheme::Compare(const std::string& Left, const std::string& Right) const
{
	const SemverParts LeftParts = ParseSemver(Left);
	const SemverParts RightParts = ParseSemver(Right);

	if (!LeftParts.Valid || !RightParts.Valid) {
		// 有一边不合法就退回逐字节比。这样调用方不会拿到一个随意的大小关系。
		const int Result = Left.compare(Right);
		if (Result < 0) {
			return VersionOrder::Less;
		}
		if (Result > 0) {
			return VersionOrder::Greater;
		}
		return VersionOrder::Equal;
	}

	if (LeftParts.Major != RightParts.Major) {
		return LeftParts.Major < RightParts.Major ? VersionOrder::Less : VersionOrder::Greater;
	}
	if (LeftParts.Minor != RightParts.Minor) {
		return LeftParts.Minor < RightParts.Minor ? VersionOrder::Less : VersionOrder::Greater;
	}
	if (LeftParts.Patch != RightParts.Patch) {
		return LeftParts.Patch < RightParts.Patch ? VersionOrder::Less : VersionOrder::Greater;
	}

	const int Result = ComparePreRelease(LeftParts.PreRelease, RightParts.PreRelease);
	if (Result < 0) {
		return VersionOrder::Less;
	}
	if (Result > 0) {
		return VersionOrder::Greater;
	}
	return VersionOrder::Equal;
}

VersionSchemeRegistry::VersionSchemeRegistry()
{
	Schemes.push_back(std::unique_ptr<VersionScheme>(new RpmEvrScheme()));
	Schemes.push_back(std::unique_ptr<VersionScheme>(new SemverScheme()));
}

const VersionScheme* VersionSchemeRegistry::Find(const std::string& Scheme) const
{
	for (const std::unique_ptr<VersionScheme>& Item : Schemes) {
		if (Item->Name() == Scheme) {
			return Item.get();
		}
	}
	return nullptr;
}

std::vector<std::string> VersionSchemeRegistry::Names() const
{
	std::vector<std::string> Result;
	Result.reserve(Schemes.size());
	for (const std::unique_ptr<VersionScheme>& Item : Schemes) {
		Result.push_back(Item->Name());
	}
	return Result;
}

int VersionSchemeRegistry::Compare(const std::string& Scheme, const std::string& Left,
	                               const std::string& Right, VersionOrder& OutOrder) const
{
	const VersionScheme* Found = Find(Scheme);
	if (Found == nullptr) {
		return -EINVAL;
	}
	OutOrder = Found->Compare(Left, Right);
	return 0;
}

} // namespace okrapm