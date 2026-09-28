#include "okrapmlib/dependency_serialization.h"

#include <cerrno>

namespace okrapm {

// 块标记。用 '@' 开头，因为 object.h 里已有字段用的是 "namespace.name@version"
// 形式，单独一行的 '@' 前缀不会和它们撞。
static const char* BlockHeader = "@dependencies";
static const char* RowPrefix = "@dependency";
static const char* BlockVersion = "1";

// EscapeField() - 把字段里的控制字符转义。
// @Text: 原始字段。
// Return: 转义后的字段。
static std::string EscapeField(const std::string& Text)
{
	std::string Result;
	Result.reserve(Text.size());
	for (char Character : Text) {
		switch (Character) {
		case '\\':
			Result += "\\\\";
			break;
		case '\t':
			Result += "\\t";
			break;
		case '\n':
			Result += "\\n";
			break;
		case '\r':
			Result += "\\r";
			break;
		default:
			Result.push_back(Character);
			break;
		}
	}
	return Result;
}

// UnescapeField() - 还原转义。
// @Text: 转义后的字段。
// Return: 还原后的字段。
static std::string UnescapeField(const std::string& Text)
{
	std::string Result;
	Result.reserve(Text.size());

	size_t Index = 0;
	while (Index < Text.size()) {
		const char Character = Text[Index];
		if (Character != '\\' || Index + 1 >= Text.size()) {
			Result.push_back(Character);
			Index++;
			continue;
		}

		const char Next = Text[Index + 1];
		switch (Next) {
		case '\\':
			Result.push_back('\\');
			break;
		case 't':
			Result.push_back('\t');
			break;
		case 'n':
			Result.push_back('\n');
			break;
		case 'r':
			Result.push_back('\r');
			break;
		default:
			// 不认识的转义原样保留，避免吃掉内容。
			Result.push_back('\\');
			Result.push_back(Next);
			break;
		}
		Index += 2;
	}
	return Result;
}

// SplitFields() - 按制表符切分一行。
// @Line: 一行文本。
// Return: 切好的字段。
static std::vector<std::string> SplitFields(const std::string& Line)
{
	std::vector<std::string> Fields;
	std::string Current;
	for (char Character : Line) {
		if (Character == '\t') {
			Fields.push_back(Current);
			Current.clear();
		} else {
			Current.push_back(Character);
		}
	}
	Fields.push_back(Current);
	return Fields;
}

// SplitLines() - 按换行切分文本。
// @Text: 待切分文本。
// Return: 切好的行。
static std::vector<std::string> SplitLines(const std::string& Text)
{
	std::vector<std::string> Lines;
	std::string Current;
	for (char Character : Text) {
		if (Character == '\n') {
			Lines.push_back(Current);
			Current.clear();
		} else {
			Current.push_back(Character);
		}
	}
	if (!Current.empty()) {
		Lines.push_back(Current);
	}
	return Lines;
}

// StartsWith() - 判断前缀。
// @Text: 文本。
// @Prefix: 前缀。
// Return: 匹配返回 true。
static bool StartsWith(const std::string& Text, const char* Prefix)
{
	const std::string PrefixText = Prefix;
	if (Text.size() < PrefixText.size()) {
		return false;
	}
	return Text.compare(0, PrefixText.size(), PrefixText) == 0;
}

std::string SerializeDependencies(const std::vector<Dependency>& Items)
{
	// 空列表序列化成空串，这样旧对象的输出字节不变。
	if (Items.empty()) {
		return std::string();
	}

	std::string Result;
	Result += BlockHeader;
	Result += '\t';
	Result += BlockVersion;
	Result += '\n';

	for (const Dependency& Item : Items) {
		Result += RowPrefix;
		Result += '\t';
		Result += DependencyKindText(Item.Kind);
		Result += '\t';
		Result += DependencySourceText(Item.Source);
		Result += '\t';
		Result += EscapeField(Item.Namespace);
		Result += '\t';
		Result += EscapeField(Item.Name);
		Result += '\t';
		Result += ComparisonOperatorText(Item.Operator);
		Result += '\t';
		Result += EscapeField(Item.Version);
		Result += '\n';
	}
	return Result;
}

int ParseDependencies(const std::string& Text, std::vector<Dependency>& OutItems)
{
	OutItems.clear();

	const std::vector<std::string> Lines = SplitLines(Text);

	// 先找头。找不到就当作"没有依赖块"，这是合法的。
	bool HasHeader = false;
	for (const std::string& Line : Lines) {
		if (!StartsWith(Line, BlockHeader)) {
			continue;
		}

		const std::vector<std::string> Fields = SplitFields(Line);
		if (Fields.size() < 2) {
			return -EINVAL;
		}
		if (Fields[1] != BlockVersion) {
			return -EINVAL;
		}
		HasHeader = true;
		break;
	}

	if (!HasHeader) {
		// 没有头，但出现了行前缀，说明格式坏了。
		for (const std::string& Line : Lines) {
			if (StartsWith(Line, RowPrefix)) {
				return -EINVAL;
			}
		}
		return 0;
	}

	for (const std::string& Line : Lines) {
		if (!StartsWith(Line, RowPrefix)) {
			continue;
		}

		const std::vector<std::string> Fields = SplitFields(Line);
		// 7 列：前缀、Kind、Source、Namespace、Name、Operator、Version。
		// 多于 7 列时忽略多余的，便于以后加字段。
		if (Fields.size() < 7) {
			return -EINVAL;
		}

		Dependency Item;
		if (ParseDependencyKind(Fields[1], Item.Kind) != 0) {
			return -EINVAL;
		}
		if (ParseDependencySource(Fields[2], Item.Source) != 0) {
			return -EINVAL;
		}
		Item.Namespace = UnescapeField(Fields[3]);
		Item.Name = UnescapeField(Fields[4]);
		if (ParseComparisonOperator(Fields[5], Item.Operator) != 0) {
			return -EINVAL;
		}
		Item.Version = UnescapeField(Fields[6]);

		OutItems.push_back(Item);
	}
	return 0;
}

bool LooksLikeDependencyBlock(const std::string& Text)
{
	const std::vector<std::string> Lines = SplitLines(Text);
	for (const std::string& Line : Lines) {
		if (StartsWith(Line, BlockHeader) || StartsWith(Line, RowPrefix)) {
			return true;
		}
	}
	return false;
}

} // namespace okrapm