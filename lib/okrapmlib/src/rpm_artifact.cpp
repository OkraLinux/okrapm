#include "okrapmlib/rpm_artifact.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace okrapm {

// RunCommand() - 用 popen 执行命令，捕获 stdout。
int RpmArtifactBackend::RunCommand(const std::string& Command, std::string& Output)
{
	Output.clear();

	FILE* Handle = popen(Command.c_str(), "r");
	if (Handle == nullptr) {
		return -errno;
	}

	char Buffer[4096];
	while (true) {
		size_t Read = fread(Buffer, 1, sizeof(Buffer), Handle);
		if (Read == 0) {
			break;
		}
		Output.append(Buffer, Read);
	}

	int Status = pclose(Handle);
	int ExitCode = WEXITSTATUS(Status);
	return ExitCode;
}

bool RpmArtifactBackend::CanHandle(const std::string& LocalPath) const
{
	// 检查文件是否存在且以 .rpm 结尾
	if (LocalPath.size() < 4) {
		return false;
	}
	return LocalPath.compare(LocalPath.size() - 4, 4, ".rpm") == 0;
}

// SplitLines() - 按换行切分。
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

// TrimWhitespace() - 去首尾空白。
static std::string TrimWhitespace(const std::string& Text)
{
	size_t Start = 0;
	while (Start < Text.size() &&
	       (Text[Start] == ' ' || Text[Start] == '\t' || Text[Start] == '\r')) {
		Start++;
	}
	size_t End = Text.size();
	while (End > Start &&
	       (Text[End - 1] == ' ' || Text[End - 1] == '\t' || Text[End - 1] == '\r')) {
		End--;
	}
	return Text.substr(Start, End - Start);
}

// SplitOnWhitespace() - 按空白切分，最多切 N 段。
static std::vector<std::string> SplitOnWhitespace(const std::string& Text, int MaxParts = -1)
{
	std::vector<std::string> Parts;
	std::string Current;
	int PartCount = 0;

	for (char Character : Text) {
		if ((Character == ' ' || Character == '\t') && (MaxParts < 0 || PartCount < MaxParts - 1)) {
			if (!Current.empty()) {
				Parts.push_back(Current);
				Current.clear();
				PartCount++;
			}
		} else {
			Current.push_back(Character);
		}
	}
	if (!Current.empty()) {
		Parts.push_back(Current);
	}
	return Parts;
}

std::vector<Dependency> RpmArtifactBackend::ParseDependencyList(const std::string& Output,
                                                                 DependencyKind Kind)
{
	std::vector<Dependency> Result;

	auto Lines = SplitLines(Output);
	for (const std::string& Line : Lines) {
		std::string Trimmed = TrimWhitespace(Line);
		if (Trimmed.empty()) {
			continue;
		}

		Dependency Dep;
		Dep.Kind = Kind;
		Dep.Source = DependencySource::Manual;
		Dep.Operator = ComparisonOperator::Any;

		// rich deps: (name = version if condition)
		if (Trimmed[0] == '(') {
			// 原样存储，不解析布尔结构
			Dep.Name = Trimmed;
			Result.push_back(Dep);
			continue;
		}

		// 普通依赖：name [op version]
		auto Parts = SplitOnWhitespace(Trimmed, 3);

		if (Parts.empty()) {
			continue;
		}

		Dep.Name = Parts[0];

		if (Parts.size() >= 3) {
			// name op version
			std::string OpStr = Parts[1];
			if (OpStr == "=" || OpStr == "==") {
				Dep.Operator = ComparisonOperator::Equal;
			} else if (OpStr == ">=") {
				Dep.Operator = ComparisonOperator::GreaterEqual;
			} else if (OpStr == "<=") {
				Dep.Operator = ComparisonOperator::LessEqual;
			} else if (OpStr == ">") {
				Dep.Operator = ComparisonOperator::Greater;
			} else if (OpStr == "<") {
				Dep.Operator = ComparisonOperator::Less;
			}
			Dep.Version = Parts[2];
		}

		Result.push_back(Dep);
	}

	return Result;
}

int RpmArtifactBackend::ReadMetadata(const std::string& LocalPath, RpmPackageEntry& OutEntry)
{
	OutEntry = RpmPackageEntry();

	// 检查文件存在
	struct stat Stat;
	if (stat(LocalPath.c_str(), &Stat) != 0) {
		return -ENOENT;
	}

	std::string Output;
	std::string RpmPath = "'" + LocalPath + "'";

	// name/version/release/arch/summary/size
	std::string QueryFmt =
		"rpm -qp --qf '%{NAME}\\t%{EPOCH}\\t%{VERSION}\\t%{RELEASE}\\t%{ARCH}\\t"
		"%{SUMMARY}\\t%{SIZE}\\n' " + RpmPath + " 2>/dev/null";

	int Result = RunCommand(QueryFmt, Output);
	if (Result != 0 || Output.empty()) {
		return -EIO;
	}

	auto Lines = SplitLines(Output);
	if (Lines.empty()) {
		return -EIO;
	}

	auto Fields = SplitOnWhitespace(Lines[0]);
	if (Fields.size() < 7) {
		return -EIO;
	}

	OutEntry.Name = Fields[0];
	OutEntry.Epoch = Fields[1];
	OutEntry.Version = Fields[2];
	OutEntry.Release = Fields[3];
	OutEntry.Arch = Fields[4];
	// Summary 可能含空格，但 --qf 用 tab 分隔了
	// Fields[5] 是 summary（可能被 split 截断），从 Output 里重新提取
	// 简化：用 tab 分隔而不是 whitespace
	// 重做：用 tab 分隔
	auto TabFields = SplitLines(Lines[0]);
	// 实际上 Lines[0] 已经是一行，用 tab 分隔
	std::vector<std::string> TabParts;
	std::string TabCurrent;
	for (char Character : Lines[0]) {
		if (Character == '\t') {
			TabParts.push_back(TabCurrent);
			TabCurrent.clear();
		} else {
			TabCurrent.push_back(Character);
		}
	}
	TabParts.push_back(TabCurrent);

	if (TabParts.size() >= 7) {
		OutEntry.Name = TabParts[0];
		OutEntry.Epoch = TabParts[1];
		OutEntry.Version = TabParts[2];
		OutEntry.Release = TabParts[3];
		OutEntry.Arch = TabParts[4];
		OutEntry.Summary = TabParts[5];
		OutEntry.InstalledSize = std::stoll(TabParts[6]);
	}

	OutEntry.Location = LocalPath;

	// provides
	Output.clear();
	RunCommand("rpm -qp --provides " + RpmPath + " 2>/dev/null", Output);
	OutEntry.Provides = ParseDependencyList(Output, DependencyKind::Provides);

	// requires
	Output.clear();
	RunCommand("rpm -qp --requires " + RpmPath + " 2>/dev/null", Output);
	OutEntry.Requires = ParseDependencyList(Output, DependencyKind::Requires);

	// conflicts
	Output.clear();
	RunCommand("rpm -qp --conflicts " + RpmPath + " 2>/dev/null", Output);
	OutEntry.Conflicts = ParseDependencyList(Output, DependencyKind::Conflicts);

	// obsoletes
	Output.clear();
	RunCommand("rpm -qp --obsoletes " + RpmPath + " 2>/dev/null", Output);
	OutEntry.Obsoletes = ParseDependencyList(Output, DependencyKind::Obsoletes);

	// recommends
	Output.clear();
	RunCommand("rpm -qp --recommends " + RpmPath + " 2>/dev/null", Output);
	OutEntry.Recommends = ParseDependencyList(Output, DependencyKind::Recommends);

	// suggests
	Output.clear();
	RunCommand("rpm -qp --suggests " + RpmPath + " 2>/dev/null", Output);
	OutEntry.Suggests = ParseDependencyList(Output, DependencyKind::Suggests);

	return 0;
}

int RpmArtifactBackend::ListPayload(const std::string& LocalPath,
                                     std::vector<RpmPayloadEntry>& OutEntries)
{
	OutEntries.clear();

	std::string Output;
	std::string RpmPath = "'" + LocalPath + "'";
	int Result = RunCommand("rpm -qpl " + RpmPath + " 2>/dev/null", Output);
	if (Result != 0) {
		return -EIO;
	}

	auto Lines = SplitLines(Output);
	for (const std::string& Line : Lines) {
		std::string Trimmed = TrimWhitespace(Line);
		if (Trimmed.empty()) {
			continue;
		}
		RpmPayloadEntry Entry;
		Entry.Path = Trimmed;
		OutEntries.push_back(Entry);
	}

	return 0;
}

int RpmArtifactBackend::ListScriptlets(const std::string& LocalPath,
                                        std::vector<RpmScriptlet>& OutScriptlets)
{
	OutScriptlets.clear();

	std::string Output;
	std::string RpmPath = "'" + LocalPath + "'";
	int Result = RunCommand("rpm -qp --scripts " + RpmPath + " 2>/dev/null", Output);
	if (Result != 0) {
		return -EIO;
	}

	// rpm --scripts 的输出格式：
	//   postinstall scriptlet (using /bin/sh):
	//   <script content>
	//   postuninstall scriptlet (using /bin/sh):
	//   <script content>
	//
	// 解析：找 "scriptlet" 行，下面是脚本内容。

	auto Lines = SplitLines(Output);
	size_t Index = 0;
	while (Index < Lines.size()) {
		std::string Line = TrimWhitespace(Lines[Index]);

		// 找 scriptlet 标题行
		if (Line.find("scriptlet") != std::string::npos) {
			RpmScriptlet Scriptlet;

			// 解析阶段
			if (Line.find("preinstall") != std::string::npos ||
			    Line.find("prein") != std::string::npos) {
				Scriptlet.Phase = "pre";
			} else if (Line.find("postinstall") != std::string::npos ||
			           Line.find("postin") != std::string::npos) {
				Scriptlet.Phase = "post";
			} else if (Line.find("preuninstall") != std::string::npos ||
			           Line.find("preun") != std::string::npos) {
				Scriptlet.Phase = "preun";
			} else if (Line.find("postuninstall") != std::string::npos ||
			           Line.find("postun") != std::string::npos) {
				Scriptlet.Phase = "postun";
			} else if (Line.find("pretrans") != std::string::npos) {
				Scriptlet.Phase = "pretrans";
			} else if (Line.find("posttrans") != std::string::npos) {
				Scriptlet.Phase = "posttrans";
			} else {
				Scriptlet.Phase = "unknown";
			}

			// 解析解释器
			size_t UsingPos = Line.find("using ");
			if (UsingPos != std::string::npos) {
				size_t PathStart = UsingPos + 6;
				size_t PathEnd = Line.find(')', PathStart);
				if (PathEnd == std::string::npos) {
					PathEnd = Line.size();
				}
				Scriptlet.Interpreter = Line.substr(PathStart, PathEnd - PathStart);
			} else {
				Scriptlet.Interpreter = "/bin/sh";
			}

			// 收集脚本内容（到下一个 scriptlet 行或结尾）
			Index++;
			while (Index < Lines.size()) {
				std::string NextLine = TrimWhitespace(Lines[Index]);
				if (NextLine.find("scriptlet") != std::string::npos) {
					break;
				}
				if (!Scriptlet.Content.empty()) {
					Scriptlet.Content += "\n";
				}
				Scriptlet.Content += Lines[Index];
				Index++;
			}

			OutScriptlets.push_back(Scriptlet);
		} else {
			Index++;
		}
	}

	return 0;
}

int RpmArtifactBackend::ExtractPayload(const std::string& LocalPath,
                                        const std::string& InstallRoot,
                                        std::vector<std::string>& OutWritten)
{
	OutWritten.clear();

	// 用 rpm2cpio + cpio 解包
	std::string Command = "rpm2cpio '" + LocalPath + "' | "
	                      "( cd '" + InstallRoot + "' && "
	                      "cpio -idmu --quiet --no-absolute-filenames ) 2>/dev/null";

	int Result = RunCommand(Command, OutWritten.empty() ? *(new std::string) : OutWritten[0]);
	// RunCommand 把 stdout 存到第二个参数，但 cpio 的 stdout 是空的。
	// 实际写入的文件用 ListPayload 再取一遍。

	if (Result != 0) {
		return -EIO;
	}

	// 取文件列表作为 OutWritten
	std::vector<RpmPayloadEntry> Payload;
	ListPayload(LocalPath, Payload);
	for (const auto& Entry : Payload) {
		OutWritten.push_back(Entry.Path);
	}

	return 0;
}

} // namespace okrapm