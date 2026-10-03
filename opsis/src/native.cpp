#include "detail.h"

namespace Opsis {
namespace detail {

void LogLine(const char *Color, const char *Tag, const std::string &Message, std::ostream &Out)
{
	Out << Color << Tag << "\033[0m " << Message << "\n";
}

fs::path JoinInside(Interpreter &Machine, const std::string &Destination, int Line)
{
	fs::path Root = fs::path(Machine.Config().Sysroot).lexically_normal();
	fs::path Relative = fs::path(Destination).relative_path();
	if (Relative.empty()) Machine.Fail(Line, "empty destination", true);
	fs::path Full = (Root / Relative).lexically_normal();
	fs::path Back = Full.lexically_relative(Root);
	if (Back.empty() || *Back.begin() == "..") {
		Machine.Fail(Line, "destination escapes the sysroot: " + Destination, true);
	}
	return Full;
}

fs::path ResolveSource(const Interpreter &Machine, const std::string &Path)
{
	fs::path Item(Path);
	if (Item.is_absolute()) return Item;
	return Machine.ScriptDirectory() / Item;
}

int RunProcess(const std::vector<std::string> &Args, bool Quiet)
{
	pid_t Pid = fork();
	if (Pid < 0) return -1;
	if (Pid == 0) {
		if (Quiet) {
			int DevNull = open("/dev/null", O_WRONLY);
			if (DevNull >= 0) {
				dup2(DevNull, STDOUT_FILENO);
				dup2(DevNull, STDERR_FILENO);
				if (DevNull > 2) close(DevNull);
			}
		}
		std::vector<char *> Argv;
		Argv.reserve(Args.size() + 1);
		for (const auto &Arg : Args) Argv.push_back(const_cast<char *>(Arg.c_str()));
		Argv.push_back(nullptr);
		execvp(Argv[0], Argv.data());
		_exit(127);
	}
	int Status = 0;
	if (waitpid(Pid, &Status, 0) < 0) return -1;
	if (WIFEXITED(Status)) return WEXITSTATUS(Status);
	return -1;
}

void NativeCheckUser(Interpreter &Machine, int Line)
{
	if (geteuid() != 0 && !Machine.Config().AllowNonRoot) {
		Machine.Fail(Line, "OPSIS installation requires root privileges.", true);
	}
}

void NativeInstallFile(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	std::string Source = NeedString(Machine, Args[0], Line);
	std::string Destination = NeedString(Machine, Args[1], Line);
	std::string Mode = Args.size() >= 3 ? NeedString(Machine, Args[2], Line) : "0755";
	std::string Owner = Args.size() >= 4 ? NeedString(Machine, Args[3], Line) : "0:0";
	fs::path From = ResolveSource(Machine, Source);
	fs::path To = JoinInside(Machine, Destination, Line);
	if (!fs::is_regular_file(From)) Machine.Fail(Line, "source file does not exist: " + From.string(), true);
	std::error_code Error;
	fs::create_directories(To.parent_path(), Error);
	if (Error) Machine.Fail(Line, "cannot create directory " + To.parent_path().string(), true);
	fs::copy_file(From, To, fs::copy_options::overwrite_existing, Error);
	if (Error) Machine.Fail(Line, "failed to install " + From.string(), true);
	char *End = nullptr;
	unsigned long Bits = std::strtoul(Mode.c_str(), &End, 8);
	if (End && *End == '\0' && Bits <= 07777) {
		chmod(To.c_str(), static_cast<mode_t>(Bits));
	}
	auto Colon = Owner.find(':');
	if (Colon != std::string::npos) {
		std::string User = Owner.substr(0, Colon);
		std::string Group = Owner.substr(Colon + 1);
		char *UserEnd = nullptr;
		char *GroupEnd = nullptr;
		long Uid = std::strtol(User.c_str(), &UserEnd, 10);
		long Gid = std::strtol(Group.c_str(), &GroupEnd, 10);
		if (UserEnd && *UserEnd == '\0' && GroupEnd && *GroupEnd == '\0') {
			chown(To.c_str(), static_cast<uid_t>(Uid), static_cast<gid_t>(Gid));
		}
	}
	LogLine("\033[1;32m", "[OPSIS:INFO]", "  -> installed " + Destination, std::cout);
}

void NativeInstallDirectory(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	std::string Source = NeedString(Machine, Args[0], Line);
	std::string Prefix = Args.size() >= 2 ? NeedString(Machine, Args[1], Line) : "/";
	fs::path From = ResolveSource(Machine, Source);
	if (!fs::is_directory(From)) return;
	std::error_code Error;
	for (fs::recursive_directory_iterator It(From, fs::directory_options::skip_permission_denied, Error), End;
		It != End; It.increment(Error)) {
		if (Error) {
			Error.clear();
			continue;
		}
		fs::path Rel = fs::relative(It->path(), From, Error);
		if (Error || Rel.empty() || Rel.native().find("..") != std::string::npos) {
			Machine.Fail(Line, "payload path escapes the source directory", true);
		}
		fs::path To = JoinInside(Machine, (fs::path(Prefix) / Rel).generic_string(), Line);
		if (It->is_directory()) {
			fs::create_directories(To, Error);
			if (Error) Machine.Fail(Line, "cannot create directory " + To.string(), true);
			continue;
		}
		if (It->is_symlink()) {
			fs::create_directories(To.parent_path(), Error);
			fs::copy_symlink(It->path(), To, Error);
			if (Error) Machine.Fail(Line, "failed to copy symlink " + Rel.string(), true);
			continue;
		}
		if (!It->is_regular_file()) continue;
		fs::create_directories(To.parent_path(), Error);
		fs::copy_file(It->path(), To, fs::copy_options::overwrite_existing, Error);
		if (Error) Machine.Fail(Line, "failed to copy " + Rel.string(), true);
	}
}

void NativeRecord(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	std::string Name = NeedString(Machine, Args[0], Line);
	std::string Version = NeedString(Machine, Args[1], Line);
	std::string Manifest = NeedString(Machine, Args[2], Line);
	if (Name.empty() || Name.find('/') != std::string::npos || Name.find("..") != std::string::npos) {
		Machine.Fail(Line, "invalid package name", true);
	}
	fs::path Dir = fs::path(Machine.Config().DatabaseDirectory) / Name;
	std::error_code Error;
	fs::create_directories(Dir, Error);
	if (Error) Machine.Fail(Line, "cannot create package record", true);
	{
		std::ofstream VersionFile(Dir / "version");
		if (!VersionFile) Machine.Fail(Line, "cannot write package version", true);
		VersionFile << Version << "\n";
	}
	{
		std::time_t Now = std::time(nullptr);
		std::tm Utc{};
		gmtime_r(&Now, &Utc);
		char Buffer[32];
		std::strftime(Buffer, sizeof(Buffer), "%Y-%m-%dT%H:%M:%SZ", &Utc);
		std::ofstream TimeFile(Dir / "installed_time");
		if (!TimeFile) Machine.Fail(Line, "cannot write install time", true);
		TimeFile << Buffer << "\n";
	}
	fs::path ManifestPath = ResolveSource(Machine, Manifest);
	if (fs::is_regular_file(ManifestPath)) {
		fs::copy_file(ManifestPath, Dir / "manifest", fs::copy_options::overwrite_existing, Error);
		if (Error) Machine.Fail(Line, "cannot copy manifest", true);
	}
	LogLine("\033[1;32m", "[OPSIS:INFO]",
		"Recorded package " + Name + "-" + Version + " in system database.", std::cout);
}

bool ValidPackageName(const std::string &Name)
{
	return !Name.empty() && Name.find('/') == std::string::npos && Name.find("..") == std::string::npos;
}

void ApplyMode(const fs::path &Path, const std::string &Mode)
{
	char *End = nullptr;
	unsigned long Bits = std::strtoul(Mode.c_str(), &End, 8);
	if (End && *End == '\0' && Bits <= 07777) chmod(Path.c_str(), static_cast<mode_t>(Bits));
}

void WriteText(Interpreter &Machine, const std::string &Destination, const std::string &Text, bool Append, int Line)
{
	fs::path To = JoinInside(Machine, Destination, Line);
	std::error_code Error;
	fs::create_directories(To.parent_path(), Error);
	if (Error) Machine.Fail(Line, "cannot create directory " + To.parent_path().string(), true);
	std::ofstream Output(To, Append ? std::ios::app : std::ios::trunc);
	if (!Output) Machine.Fail(Line, "cannot write " + Destination, true);
	Output << Text;
	if (!Output) Machine.Fail(Line, "cannot write " + Destination, true);
	LogLine("\033[1;32m", "[OPSIS:INFO]", "  -> wrote " + Destination, std::cout);
}

bool AllDigits(const std::string &Text)
{
	if (Text.empty()) return false;
	for (unsigned char Ch : Text) {
		if (!std::isdigit(Ch)) return false;
	}
	return true;
}

int CompareVersionText(const std::string &Left, const std::string &Right)
{
	auto Part = [](const std::string &Text) {
		std::vector<std::string> Parts;
		std::string Current;
		for (char Ch : Text) {
			if (Ch == '.') {
				Parts.push_back(Current);
				Current.clear();
			} else {
				Current.push_back(Ch);
			}
		}
		Parts.push_back(Current);
		return Parts;
	};
	std::vector<std::string> A = Part(Left);
	std::vector<std::string> B = Part(Right);
	size_t Count = A.size() > B.size() ? A.size() : B.size();
	for (size_t Index = 0; Index < Count; ++Index) {
		std::string As = Index < A.size() ? A[Index] : "0";
		std::string Bs = Index < B.size() ? B[Index] : "0";
		if (AllDigits(As) && AllDigits(Bs)) {
			try {
				int64_t Ai = std::stoll(As);
				int64_t Bi = std::stoll(Bs);
				if (Ai < Bi) return -1;
				if (Ai > Bi) return 1;
				continue;
			} catch (...) {
			}
		}
		int Cmp = As.compare(Bs);
		if (Cmp < 0) return -1;
		if (Cmp > 0) return 1;
	}
	return 0;
}

bool CommandExists(const std::string &Name)
{
	std::string Probe = "command -v " + Name + " >/dev/null 2>&1";
	return std::system(Probe.c_str()) == 0;
}

void RunHostTool(Interpreter &Machine, const std::string &Tool, const std::string &Relative, const std::string &Step)
{
	fs::path Dir = fs::path(Machine.Config().Sysroot) / Relative;
	if (!fs::is_directory(Dir) || !CommandExists(Tool)) return;
	LogLine("\033[1;36m", "[OPSIS:STEP]", Step, std::cout);
	RunProcess({Tool, Dir.string()}, true);
}

DirectPackage PackageSession;

void ResetDirectPackage()
{
	if (!PackageSession.Stage.empty()) {
		std::error_code Error;
		fs::remove_all(PackageSession.Stage, Error);
	}
	PackageSession = {};
}

std::string ConfigOrEnv(const std::string &Field, const char *Name)
{
	if (!Field.empty()) return Field;
	const char *Value = std::getenv(Name);
	if (Value && *Value) return Value;
	return "";
}

bool PackageToken(const std::string &Text)
{
	if (Text.empty() || !std::isalnum(static_cast<unsigned char>(Text[0]))) return false;
	for (unsigned char Ch : Text) {
		if (std::isalnum(Ch) || Ch == '.' || Ch == '_' || Ch == '-' || Ch == '+') continue;
		return false;
	}
	return true;
}

fs::path PackageRelative(Interpreter &Machine, const std::string &Destination, int Line)
{
	fs::path Item(Destination);
	if (!Item.is_absolute()) Machine.Fail(Line, "package path must be absolute: " + Destination, true);
	fs::path Relative = Item.relative_path().lexically_normal();
	if (Relative.empty() || *Relative.begin() == "..") {
		Machine.Fail(Line, "package path escapes the archive: " + Destination, true);
	}
	return Relative;
}

fs::path BuildSource(const Interpreter &Machine, const std::string &Path)
{
	fs::path Item(Path);
	if (Item.is_absolute()) return Item;
	if (!Machine.Config().BuildDirectory.empty()) {
		fs::path Built = fs::path(Machine.Config().BuildDirectory) / Item;
		if (fs::exists(Built)) return Built;
	}
	return Machine.ScriptDirectory() / Item;
}

void RequireOpen(Interpreter &Machine, int Line)
{
	if (!PackageSession.Open) Machine.Fail(Line, "BeginPackage was not called", true);
}

void CopyPackedFile(Interpreter &Machine, const fs::path &From, const fs::path &Relative, const std::string &Mode,
	int Line)
{
	fs::path To = PackageSession.Stage / "rootfs" / Relative;
	std::error_code Error;
	fs::create_directories(To.parent_path(), Error);
	if (Error) Machine.Fail(Line, "cannot create directory " + To.parent_path().string(), true);
	fs::copy_file(From, To, fs::copy_options::overwrite_existing, Error);
	if (Error) Machine.Fail(Line, "cannot pack " + From.string(), true);
	ApplyMode(To, Mode);
	std::string Listed = "/" + Relative.generic_string();
	PackageSession.Files.push_back(Listed);
	PackageSession.InstalledSize += static_cast<uint64_t>(fs::file_size(To, Error));
}

int WriteArchive(const fs::path &Stage, const fs::path &Output)
{
	std::error_code Error;
	fs::remove(Output, Error);
	int Status = RunProcess({"tar", "--zstd", "-cf", Output.string(), "-C", Stage.string(), "."}, true);
	if (Status == 0 && fs::is_regular_file(Output)) return 0;
	fs::remove(Output, Error);
	Status = RunProcess({"tar", "-czf", Output.string(), "-C", Stage.string(), "."}, true);
	if (Status == 0 && fs::is_regular_file(Output)) return 0;
	fs::remove(Output, Error);
	return -1;
}

void WriteChecksum(const fs::path &Output)
{
	if (Output.string().find('\'') != std::string::npos) return;
	std::string Command = "sha256sum '" + Output.string() + "'";
	FILE *Pipe = popen(Command.c_str(), "r");
	if (!Pipe) return;
	char Buffer[128];
	std::string Sum;
	if (std::fgets(Buffer, sizeof(Buffer), Pipe)) Sum = Buffer;
	pclose(Pipe);
	auto Space = Sum.find(' ');
	if (Space == std::string::npos || Space == 0) return;
	std::ofstream Sidecar(Output.string() + ".sha256");
	if (!Sidecar) return;
	Sidecar << Sum.substr(0, Space) << "  " << Output.filename().string() << "\n";
}

void BeginDirectPackage(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	if (PackageSession.Open) Machine.Fail(Line, "a package is already open", true);
	std::string Namespace = ConfigOrEnv(Machine.Config().PackageNamespace, "OPSIS_PKG_NAMESPACE");
	std::string Name = ConfigOrEnv(Machine.Config().PackageName, "OPSIS_PKG_NAME");
	std::string Version = ConfigOrEnv(Machine.Config().PackageVersion, "OPSIS_PKG_VERSION");
	std::string Output = ConfigOrEnv(Machine.Config().PackageOutput, "OPSIS_PKG_OUTPUT");
	if (Args.size() >= 3) {
		Namespace = NeedString(Machine, Args[0], Line);
		Name = NeedString(Machine, Args[1], Line);
		Version = NeedString(Machine, Args[2], Line);
	}
	if (Args.size() >= 4) Output = NeedString(Machine, Args[3], Line);
	if (Namespace.empty()) Namespace = "Okra";
	if (!PackageToken(Namespace) || !PackageToken(Name) || !PackageToken(Version)) {
		Machine.Fail(Line, "package name, namespace, or version is empty or unsafe", true);
	}
	if (Output.empty()) Output = Name + "-" + Version + ".oaa";
	fs::path OutputPath = fs::absolute(Output);
	std::string Description = ConfigOrEnv(Machine.Config().PackageDescription, "OPSIS_PKG_DESCRIPTION");
	if (Description.empty()) Description = Name + " direct package";
	std::string Architecture = ConfigOrEnv(Machine.Config().PackageArchitecture, "OPSIS_PKG_ARCH");
	if (Architecture.empty()) Architecture = HostArchitecture();
	fs::path Stage = fs::temp_directory_path() / ("opsis-direct-" + std::to_string(getpid()));
	PackageSession.Stage = Stage;
	std::error_code Error;
	fs::remove_all(Stage, Error);
	fs::create_directories(Stage / "rootfs", Error);
	fs::create_directories(Stage / "scripts", Error);
	if (Error) {
		ResetDirectPackage();
		Machine.Fail(Line, "cannot create package stage", true);
	}
	PackageSession.Open = true;
	PackageSession.Namespace = Namespace;
	PackageSession.Name = Name;
	PackageSession.Version = Version;
	PackageSession.Description = Description;
	PackageSession.Architecture = Architecture;
	PackageSession.Output = OutputPath;
}

void PackOneFile(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	RequireOpen(Machine, Line);
	fs::path From = BuildSource(Machine, NeedString(Machine, Args[0], Line));
	if (!fs::is_regular_file(From)) Machine.Fail(Line, "build product does not exist: " + From.string(), true);
	fs::path Relative = PackageRelative(Machine, NeedString(Machine, Args[1], Line), Line);
	std::string Mode = Args.size() >= 3 ? NeedString(Machine, Args[2], Line) : "0755";
	CopyPackedFile(Machine, From, Relative, Mode, Line);
}

void PackOneDirectory(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	RequireOpen(Machine, Line);
	fs::path From = BuildSource(Machine, NeedString(Machine, Args[0], Line));
	if (!fs::is_directory(From)) Machine.Fail(Line, "build directory does not exist: " + From.string(), true);
	fs::path Prefix = PackageRelative(Machine, NeedString(Machine, Args[1], Line), Line);
	std::error_code Error;
	for (fs::recursive_directory_iterator It(From, fs::directory_options::skip_permission_denied, Error), End;
		It != End; It.increment(Error)) {
		if (Error) {
			Error.clear();
			continue;
		}
		if (!It->is_regular_file()) continue;
		fs::path Rel = fs::relative(It->path(), From, Error);
		if (Error || Rel.empty() || Rel.generic_string().find("..") != std::string::npos) {
			Machine.Fail(Line, "build path escapes the source directory", true);
		}
		fs::path Destination = Prefix / Rel;
		std::string Mode = (fs::status(It->path()).permissions() & fs::perms::owner_exec) != fs::perms::none
			? "0755" : "0644";
		CopyPackedFile(Machine, It->path(), Destination.lexically_normal(), Mode, Line);
	}
}

void FinishDirectPackage(Interpreter &Machine, const std::vector<Value> &Args, int Line)
{
	RequireOpen(Machine, Line);
	if (!Args.empty()) PackageSession.Output = fs::absolute(NeedString(Machine, Args[0], Line));
	std::string Description = PackageSession.Description;
	for (char &Ch : Description) {
		if (Ch == '"' || Ch == '\n') Ch = ' ';
	}
	{
		std::ofstream Meta(PackageSession.Stage / "meta.yaml");
		if (!Meta) Machine.Fail(Line, "cannot write meta.yaml", true);
		Meta << "name: " << PackageSession.Name << "\n";
		Meta << "namespace: " << PackageSession.Namespace << "\n";
		Meta << "version: " << PackageSession.Version << "\n";
		Meta << "description: \"" << Description << "\"\n";
		Meta << "architecture: " << PackageSession.Architecture << "\n";
		Meta << "maintainer: \"OkraLinux Maintainer <maintainer@okralinux.cn>\"\n";
		Meta << "installed_size: " << PackageSession.InstalledSize << "\n";
		if (!PackageSession.Dependencies.empty()) {
			Meta << "dependencies:\n";
			for (const std::string &Dep : PackageSession.Dependencies) Meta << "  - " << Dep << "\n";
		}
		if (!PackageSession.Files.empty()) {
			Meta << "files:\n";
			for (const std::string &File : PackageSession.Files) Meta << "  - " << File << "\n";
		}
	}
	{
		std::ofstream Script(PackageSession.Stage / "scripts/install.opsis");
		if (!Script) Machine.Fail(Line, "cannot write install.opsis", true);
		Script << "public class Package {\n";
		Script << "    public void INSTALL() {\n";
		Script << "        CheckUser();\n";
		Script << "        InstallDirectory(\"rootfs\", \"/\");\n";
		Script << "        UpdateLdconfig();\n";
		Script << "        RecordInstalled(\"" << PackageSession.Name << "\", \"";
		Script << PackageSession.Version << "\", \"meta.yaml\");\n";
		Script << "    }\n";
		Script << "}\n";
	}
	{
		std::ofstream Script(PackageSession.Stage / "scripts/remove.opsis");
		if (!Script) Machine.Fail(Line, "cannot write remove.opsis", true);
		Script << "public class Package {\n";
		Script << "    public void REMOVE() {\n";
		for (const std::string &File : PackageSession.Files) {
			if (File.find('"') != std::string::npos) continue;
			Script << "        if (FileExists(\"" << File << "\")) {\n";
			Script << "            RemoveFile(\"" << File << "\");\n";
			Script << "        }\n";
		}
		Script << "    }\n";
		Script << "}\n";
	}
	std::error_code Error;
	fs::create_directories(PackageSession.Output.parent_path(), Error);
	if (WriteArchive(PackageSession.Stage, PackageSession.Output) != 0) {
		Machine.Fail(Line, "cannot write " + PackageSession.Output.string(), true);
	}
	WriteChecksum(PackageSession.Output);
	LogLine("\033[1;36m", "[OPSIS:STEP]",
		"Packed " + PackageSession.Namespace + "." + PackageSession.Name + " "
		+ PackageSession.Version + " -> " + PackageSession.Output.string(), std::cout);
	ResetDirectPackage();
}

void AddNative(Environment &Globals, Module &Mod, const std::string &Name, int MinArgs, int MaxArgs,
	const std::function<Value(Interpreter &, const std::vector<Value> &)> &Fn)
{
	Value Item = Value::FromNative(Name, MinArgs, MaxArgs, Fn);
	Globals.Define(Name, Item);
	Mod.Members[Name] = Item;
}

}
}
