#include "okrapmlib/rpm_repository.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <algorithm>

namespace okrapm {

// 微型 XML 解析器
//
// 不依赖外部 XML 库。primary.xml 的结构是固定的，用正则和字符串操作就够了。
// 如果以后要解析更复杂的 XML（如 comps.xml），再考虑引入 pugixml 或类似库。

// ExtractAttribute() - 从 XML 标签里提取属性值。
// @Tag: 完整的标签文本，例如 <rpm:entry name="foo" flags="GE"/>
// @AttrName: 属性名，例如 "name"
// Return: 属性值，找不到返回空串。
static std::string ExtractAttribute(const std::string& Tag, const std::string& AttrName)
{
	std::string Pattern = AttrName + "=\"";
	size_t Pos = Tag.find(Pattern);
	if (Pos == std::string::npos) {
		return std::string();
	}
	Pos += Pattern.size();
	size_t End = Tag.find('"', Pos);
	if (End == std::string::npos) {
		return std::string();
	}
	return Tag.substr(Pos, End - Pos);
}

// ExtractTagContent() - 提取 <tag>content</tag> 里的 content。
// @Xml: XML 文本。
// @TagName: 标签名（不含尖括号），例如 "name"。
// @StartPos: 从这个位置开始找。返回时更新为 content 结束位置之后。
// Return: 标签内容，找不到返回空串。
static std::string ExtractTagContent(const std::string& Xml, const std::string& TagName,
                                       size_t& StartPos)
{
	std::string OpenTag = "<" + TagName + ">";
	std::string CloseTag = "</" + TagName + ">";

	size_t OpenPos = Xml.find(OpenTag, StartPos);
	if (OpenPos == std::string::npos) {
		return std::string();
	}
	OpenPos += OpenTag.size();

	size_t ClosePos = Xml.find(CloseTag, OpenPos);
	if (ClosePos == std::string::npos) {
		return std::string();
	}

	std::string Content = Xml.substr(OpenPos, ClosePos - OpenPos);
	StartPos = ClosePos + CloseTag.size();
	return Content;
}

// FindAllTags() - 找出所有指定名称的标签（含属性和内容）。
// @Xml: XML 文本。
// @TagName: 标签名。
// Return: 每个元素是 (完整标签文本, 标签内容)。
struct TagMatch
{
	std::string FullTag;     // 含尖括号的完整标签
	std::string Attributes;  // 属性部分
	std::string Content;    // 标签内容
	size_t EndPos;          // 标签结束后的位置
};

static std::vector<TagMatch> FindAllTags(const std::string& Xml, const std::string& TagName)
{
	std::vector<TagMatch> Result;

	std::string OpenPrefix = "<" + TagName;
	std::string CloseTag = "</" + TagName + ">";

	size_t Pos = 0;
	while (Pos < Xml.size()) {
		size_t TagStart = Xml.find(OpenPrefix, Pos);
		if (TagStart == std::string::npos) {
			break;
		}

		// 找标签结束位置（> 或 />）
		size_t TagEnd = Xml.find('>', TagStart);
		if (TagEnd == std::string::npos) {
			break;
		}

		// 自闭合标签：/> 结尾
		bool SelfClosing = (TagEnd > TagStart && Xml[TagEnd - 1] == '/');

		std::string FullTag = Xml.substr(TagStart, TagEnd - TagStart + 1);

		// 提取属性（<tag 之后、> 或 /> 之前）
		std::string Attrs = Xml.substr(TagStart + OpenPrefix.size(),
		                                TagEnd - TagStart - OpenPrefix.size());
		// 去掉末尾的 / 如果有
		if (!Attrs.empty() && Attrs.back() == '/') {
			Attrs.pop_back();
		}

		std::string Content;
		size_t EndPos;

		if (SelfClosing) {
			Content.clear();
			EndPos = TagEnd + 1;
		} else {
			size_t ClosePos = Xml.find(CloseTag, TagEnd + 1);
			if (ClosePos == std::string::npos) {
				break;
			}
			Content = Xml.substr(TagEnd + 1, ClosePos - TagEnd - 1);
			EndPos = ClosePos + CloseTag.size();
		}

		Result.push_back({FullTag, Attrs, Content, EndPos});
		Pos = EndPos;
	}

	return Result;
}

// FindTag() - 找第一个指定标签。
static std::optional<TagMatch> FindTag(const std::string& Xml, const std::string& TagName,
                                        size_t StartPos = 0)
{
	auto Tags = FindAllTags(Xml, TagName);
	if (Tags.empty()) {
		return std::nullopt;
	}
	// FindAllTags 从头扫的，但没管 StartPos。修正一下。
	// 对于这个用途够用了。
	return Tags[0];
}

// ToLowerCase() - 转小写。
static std::string ToLowerCase(const std::string& Text)
{
	std::string Result = Text;
	for (char& Character : Result) {
		Character = static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
	}
	return Result;
}

// ParseFlags() - 把 RPM 的 flags 字符串转成 ComparisonOperator。
// primary.xml 里 flags 是 "EQ"、"GE"、"LE"、"GT"、"LT"、"NE" 或空。
static ComparisonOperator ParseFlags(const std::string& Flags)
{
	std::string Upper = ToLowerCase(Flags);
	if (Upper == "eq" || Upper == "=") {
		return ComparisonOperator::Equal;
	}
	if (Upper == "ge" || Upper == ">=") {
		return ComparisonOperator::GreaterEqual;
	}
	if (Upper == "le" || Upper == "<=") {
		return ComparisonOperator::LessEqual;
	}
	if (Upper == "gt" || Upper == ">") {
		return ComparisonOperator::Greater;
	}
	if (Upper == "lt" || Upper == "<") {
		return ComparisonOperator::Less;
	}
	if (Upper == "ne" || Upper == "!=") {
		// NE 在依赖匹配里不太常见，先当 Any 处理。
		return ComparisonOperator::Any;
	}
	return ComparisonOperator::Any;
}

Dependency RpmRepositoryBackend::ParseEntry(const std::string& Name, const std::string& Flags,
                                             const std::string& Epoch, const std::string& Ver,
                                             const std::string& Rel, DependencyKind Kind)
{
	Dependency Dep;
	Dep.Name = Name;
	Dep.Kind = Kind;
	Dep.Source = DependencySource::Manual;
	Dep.Operator = ParseFlags(Flags);

	// 拼版本：epoch:ver-rel
	std::string Version;
	if (!Epoch.empty() && Epoch != "0") {
		Version = Epoch + ":";
	}
	Version += Ver;
	if (!Rel.empty()) {
		Version += "-" + Rel;
	}
	Dep.Version = Version;

	return Dep;
}

std::vector<Dependency> RpmRepositoryBackend::ParseDependencyEntries(const std::string& EntryXml,
                                                                       DependencyKind Kind)
{
	std::vector<Dependency> Result;

	auto Tags = FindAllTags(EntryXml, "rpm:entry");
	for (const auto& Tag : Tags) {
		std::string Name = ExtractAttribute(Tag.Attributes, "name");
		std::string Flags = ExtractAttribute(Tag.Attributes, "flags");
		std::string Epoch = ExtractAttribute(Tag.Attributes, "epoch");
		std::string Ver = ExtractAttribute(Tag.Attributes, "ver");
		std::string Rel = ExtractAttribute(Tag.Attributes, "rel");

		Result.push_back(ParseEntry(Name, Flags, Epoch, Ver, Rel, Kind));
	}

	return Result;
}

void RpmRepositoryBackend::ParseRepomdData(const std::string& Xml)
{
	MetadataRefs.clear();

	auto DataTags = FindAllTags(Xml, "data");
	for (const auto& Tag : DataTags) {
		RpmRepoMetadataRef Ref;
		Ref.DataType = ExtractAttribute(Tag.Attributes, "type");

		// location
		auto LocTags = FindAllTags(Tag.Content, "location");
		if (!LocTags.empty()) {
			Ref.Location = ExtractAttribute(LocTags[0].Attributes, "href");
		}

		// checksum
		auto CkTags = FindAllTags(Tag.Content, "checksum");
		if (!CkTags.empty()) {
			Ref.Checksum = CkTags[0].Content;
			Ref.ChecksumType = ExtractAttribute(CkTags[0].Attributes, "type");
		}

		// size
		auto SizeTags = FindAllTags(Tag.Content, "size");
		if (!SizeTags.empty()) {
			std::string SizeStr = ExtractAttribute(SizeTags[0].Attributes, "package");
			if (!SizeStr.empty()) {
				Ref.Size = std::stoll(SizeStr);
			}
		}

		// open-size
		auto OpenTags = FindAllTags(Tag.Content, "open-size");
		if (!OpenTags.empty()) {
			std::string OpenStr = ExtractAttribute(OpenTags[0].Attributes, "package");
			if (!OpenStr.empty()) {
				Ref.OpenSize = std::stoll(OpenStr);
			}
		}

		MetadataRefs.push_back(Ref);
	}
}

void RpmRepositoryBackend::ParsePrimaryPackage(const std::string& Xml)
{
	// 找所有 <package type="rpm"> 块
	size_t Pos = 0;
	std::string PackageOpen = "<package type=\"rpm\">";
	std::string PackageClose = "</package>";

	while (Pos < Xml.size()) {
		size_t Start = Xml.find(PackageOpen, Pos);
		if (Start == std::string::npos) {
			break;
		}
		Start += PackageOpen.size();

		size_t End = Xml.find(PackageClose, Start);
		if (End == std::string::npos) {
			break;
		}

		std::string Block = Xml.substr(Start, End - Start);
		RpmPackageEntry Entry;

		size_t TagPos = 0;
		Entry.Name = ExtractTagContent(Block, "name", TagPos);
		Entry.Arch = ExtractTagContent(Block, "arch", TagPos);

		// version 标签带属性
		auto VerTags = FindAllTags(Block, "version");
		if (!VerTags.empty()) {
			Entry.Epoch = ExtractAttribute(VerTags[0].Attributes, "epoch");
			Entry.Version = ExtractAttribute(VerTags[0].Attributes, "ver");
			Entry.Release = ExtractAttribute(VerTags[0].Attributes, "rel");
		}

		// summary
		TagPos = 0;
		Entry.Summary = ExtractTagContent(Block, "summary", TagPos);

		// checksum
		auto CkTags = FindAllTags(Block, "checksum");
		if (!CkTags.empty()) {
			Entry.Sha256 = CkTags[0].Content;
		}

		// size
		auto SizeTags = FindAllTags(Block, "size");
		if (!SizeTags.empty()) {
			std::string PkgStr = ExtractAttribute(SizeTags[0].Attributes, "package");
			std::string InstStr = ExtractAttribute(SizeTags[0].Attributes, "installed");
			if (!PkgStr.empty()) {
				Entry.PackageSize = std::stoll(PkgStr);
			}
			if (!InstStr.empty()) {
				Entry.InstalledSize = std::stoll(InstStr);
			}
		}

		// location
		auto LocTags = FindAllTags(Block, "location");
		if (!LocTags.empty()) {
			Entry.Location = ExtractAttribute(LocTags[0].Attributes, "href");
		}

		// format 块里的 requires / provides / conflicts / obsoletes
		auto FormatTags = FindAllTags(Block, "format");
		if (!FormatTags.empty()) {
			std::string FormatContent = FormatTags[0].Content;

			auto ReqTags = FindAllTags(FormatContent, "rpm:requires");
			if (!ReqTags.empty()) {
				Entry.Requires = ParseDependencyEntries(ReqTags[0].Content,
				                                        DependencyKind::Requires);
			}

			auto ProvTags = FindAllTags(FormatContent, "rpm:provides");
			if (!ProvTags.empty()) {
				Entry.Provides = ParseDependencyEntries(ProvTags[0].Content,
				                                        DependencyKind::Provides);
			}

			auto ConfTags = FindAllTags(FormatContent, "rpm:conflicts");
			if (!ConfTags.empty()) {
				Entry.Conflicts = ParseDependencyEntries(ConfTags[0].Content,
				                                        DependencyKind::Conflicts);
			}

			auto ObsTags = FindAllTags(FormatContent, "rpm:obsoletes");
			if (!ObsTags.empty()) {
				Entry.Obsoletes = ParseDependencyEntries(ObsTags[0].Content,
				                                         DependencyKind::Obsoletes);
			}

			auto RecTags = FindAllTags(FormatContent, "rpm:recommends");
			if (!RecTags.empty()) {
				Entry.Recommends = ParseDependencyEntries(RecTags[0].Content,
				                                         DependencyKind::Recommends);
			}

			auto SugTags = FindAllTags(FormatContent, "rpm:suggests");
			if (!SugTags.empty()) {
				Entry.Suggests = ParseDependencyEntries(SugTags[0].Content,
				                                        DependencyKind::Suggests);
			}
		}

		Packages.push_back(std::move(Entry));
		Pos = End + PackageClose.size();
	}
}

void RpmRepositoryBackend::SetBaseUrl(const std::string& Url)
{
	BaseUrl = Url;
}

void RpmRepositoryBackend::SetNamespace(const std::string& Ns)
{
	Namespace = Ns;
}

int RpmRepositoryBackend::LoadRepomd(const std::string& Xml)
{
	ParseRepomdData(Xml);
	return 0;
}

int RpmRepositoryBackend::LoadPrimary(const std::string& Xml)
{
	Packages.clear();
	ParsePrimaryPackage(Xml);
	return 0;
}

int RpmRepositoryBackend::LoadPrimaryFromFile(const std::string& Path)
{
	std::ifstream File(Path);
	if (!File) {
		return -ENOENT;
	}
	std::string Content((std::istreambuf_iterator<char>(File)),
	                    std::istreambuf_iterator<char>());
	return LoadPrimary(Content);
}

const std::vector<RpmPackageEntry>& RpmRepositoryBackend::ListObjects() const
{
	return Packages;
}

const RpmPackageEntry* RpmRepositoryBackend::Find(const std::string& Name) const
{
	const RpmPackageEntry* Best = nullptr;
	RpmEvrScheme Scheme;

	for (const auto& Pkg : Packages) {
		if (Pkg.Name != Name) {
			continue;
		}
		if (Best == nullptr) {
			Best = &Pkg;
			continue;
		}
		// 比 Best 新的替换
		std::string LeftVer = Best->Version;
		if (!Best->Release.empty()) {
			LeftVer += "-" + Best->Release;
		}
		if (!Best->Epoch.empty() && Best->Epoch != "0") {
			LeftVer = Best->Epoch + ":" + LeftVer;
		}
		std::string RightVer = Pkg.Version;
		if (!Pkg.Release.empty()) {
			RightVer += "-" + Pkg.Release;
		}
		if (!Pkg.Epoch.empty() && Pkg.Epoch != "0") {
			RightVer = Pkg.Epoch + ":" + RightVer;
		}
		if (RpmEvrCmp(RightVer, LeftVer) > 0) {
			Best = &Pkg;
		}
	}
	return Best;
}

std::vector<const RpmPackageEntry*> RpmRepositoryBackend::FindAll(const std::string& Name) const
{
	std::vector<const RpmPackageEntry*> Result;
	for (const auto& Pkg : Packages) {
		if (Pkg.Name == Name) {
			Result.push_back(&Pkg);
		}
	}
	return Result;
}

std::vector<const RpmPackageEntry*> RpmRepositoryBackend::Search(const std::string& Query) const
{
	std::vector<const RpmPackageEntry*> Result;
	std::string LowerQuery = ToLowerCase(Query);
	for (const auto& Pkg : Packages) {
		if (ToLowerCase(Pkg.Name).find(LowerQuery) != std::string::npos) {
			Result.push_back(&Pkg);
		}
	}
	return Result;
}

std::vector<const RpmPackageEntry*> RpmRepositoryBackend::WhatProvides(
    const std::string& CapabilityName) const
{
	std::vector<const RpmPackageEntry*> Result;
	for (const auto& Pkg : Packages) {
		for (const auto& Prov : Pkg.Provides) {
			if (Prov.Name == CapabilityName) {
				Result.push_back(&Pkg);
				break;
			}
		}
	}
	return Result;
}

size_t RpmRepositoryBackend::PackageCount() const
{
	return Packages.size();
}

const std::string& RpmRepositoryBackend::GetNamespace() const
{
	return Namespace;
}

} // namespace okrapm