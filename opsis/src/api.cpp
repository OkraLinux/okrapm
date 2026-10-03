#include "opsis/interpreter.h"

#include "detail.h"

namespace Opsis {

using namespace detail;

const char *HostArchitecture()
{
#if defined(__aarch64__)
	return "aarch64";
#elif defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 64)
	return "riscv64";
#elif defined(__x86_64__)
	return "x86_64";
#else
	static std::string Name;
	if (Name.empty()) {
		utsname Info{};
		if (uname(&Info) == 0 && Info.machine[0] != '\0') Name = Info.machine;
		else Name = "unknown";
	}
	return Name.c_str();
#endif
}

RuntimeConfig LoadConfig()
{
	RuntimeConfig Config;
	const char *Sysroot = std::getenv("OPSIS_SYSROOT");
	Config.Sysroot = Sysroot && *Sysroot ? Sysroot : "/";
	const char *Allow = std::getenv("OPSIS_ALLOW_NONROOT");
	Config.AllowNonRoot = Allow && std::string(Allow) == "1";
	const char *Database = std::getenv("OPSIS_DB_DIR");
	if (Database && *Database) Config.DatabaseDirectory = Database;
	else Config.DatabaseDirectory = (fs::path(Config.Sysroot) / "var/lib/okrapm/db").string();
	auto Take = [](const char *Name) {
		const char *Value = std::getenv(Name);
		return Value && *Value ? std::string(Value) : std::string();
	};
	Config.PackageNamespace = Take("OPSIS_PKG_NAMESPACE");
	Config.PackageName = Take("OPSIS_PKG_NAME");
	Config.PackageVersion = Take("OPSIS_PKG_VERSION");
	Config.PackageDescription = Take("OPSIS_PKG_DESCRIPTION");
	Config.PackageArchitecture = Take("OPSIS_PKG_ARCH");
	Config.PackageOutput = Take("OPSIS_PKG_OUTPUT");
	Config.BuildDirectory = Take("OPSIS_BUILD_DIR");
	return Config;
}

void ApplyScene(RuntimeConfig &Config, const PackageScene &Scene)
{
	if (!Scene.Namespace.empty()) Config.PackageNamespace = Scene.Namespace;
	if (!Scene.Name.empty()) Config.PackageName = Scene.Name;
	if (!Scene.Version.empty()) Config.PackageVersion = Scene.Version;
	if (!Scene.OldVersion.empty()) Config.OldVersion = Scene.OldVersion;
	if (!Scene.NewVersion.empty()) Config.NewVersion = Scene.NewVersion;
	if (!Scene.Architecture.empty()) Config.PackageArchitecture = Scene.Architecture;
	if (!Scene.Reason.empty()) Config.Reason = Scene.Reason;
}

RuntimeConfig SceneConfig(const std::string &Entry, const std::string &Sysroot, bool AllowNonRoot,
	const PackageScene &Scene)
{
	RuntimeConfig Config = LoadConfig();
	Config.Sysroot = Sysroot;
	Config.AllowNonRoot = AllowNonRoot || Config.AllowNonRoot;
	if (geteuid() != 0 && Sysroot != "/") Config.AllowNonRoot = true;
	const char *Database = std::getenv("OPSIS_DB_DIR");
	if (!(Database && *Database)) {
		Config.DatabaseDirectory = (fs::path(Sysroot) / "var/lib/okrapm/db").string();
	}
	Config.EntryName = CanonicalEntry(Entry);
	ApplyScene(Config, Scene);
	if (Config.Reason.empty()) Config.Reason = Config.EntryName;
	return Config;
}

int RunSource(const std::string &Source, const std::string &FileName, const RuntimeConfig &Config)
{
	try {
		RuntimeConfig Local = Config;
		if (Local.PackOnly && Local.EntryName.empty()) Local.EntryName = "MAIN";
		if (!Local.PackOnly && !fs::is_directory(Local.Sysroot)) {
			std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m Target sysroot does not exist: " << Local.Sysroot << "\n";
			return 1;
		}
		std::error_code Error;
		if (!Local.PackOnly) fs::create_directories(Local.DatabaseDirectory, Error);
		if (Error) {
			std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m Cannot create database directory: "
				<< Local.DatabaseDirectory << "\n";
			return 1;
		}
		auto Items = ParseSource(Source, FileName);
		Interpreter Machine(Local, FileName);
		Machine.Run(Items);
		return 0;
	} catch (const ReturnSignal &) {
		return 0;
	} catch (const ThrowSignal &Signal) {
		std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m " << Signal.TypeName;
		if (!Signal.Message.empty()) std::cerr << ": " << Signal.Message;
		std::cerr << "\n";
		return 1;
	} catch (const BreakSignal &) {
		std::cerr << FileName << ": break is outside a loop\n";
		return 2;
	} catch (const ContinueSignal &) {
		std::cerr << FileName << ": continue is outside a loop\n";
		return 2;
	} catch (const GotoCaseSignal &) {
		std::cerr << FileName << ": goto case is outside a switch\n";
		return 2;
	} catch (const RuntimeError &Error) {
		if (!Error.Printed) std::cerr << Error.what() << "\n";
		return Error.Code;
	}
}

int RunFile(const std::string &Path, RuntimeConfig Config)
{
	std::ifstream Input(Path);
	if (!Input) {
		std::cerr << "opsis: cannot read " << Path << "\n";
		return 2;
	}
	std::stringstream Buffer;
	Buffer << Input.rdbuf();
	Config.ScriptPath = Path;
	return RunSource(Buffer.str(), Path, Config);
}

int RunScriptFile(const std::string &ScriptPath, const std::string &Entry, const std::string &Sysroot,
	bool AllowNonRoot, const PackageScene &Scene)
{
	if (!fs::is_regular_file(ScriptPath)) return 0;
	RuntimeConfig Config = SceneConfig(Entry, Sysroot, AllowNonRoot, Scene);
	Config.BaseDirectory = fs::absolute(ScriptPath).parent_path().string();
	return RunFile(ScriptPath, Config);
}

int RunLifecycleScript(const std::string &PackageDir, const std::string &Entry, const std::string &Sysroot,
	bool AllowNonRoot, const PackageScene &Scene)
{
	fs::path Script = FindEntryProgram(PackageDir, Entry);
	if (Script.empty()) return 0;
	RuntimeConfig Config = SceneConfig(Entry, Sysroot, AllowNonRoot, Scene);
	Config.BaseDirectory = fs::absolute(PackageDir).string();
	return RunFile(Script.string(), Config);
}

int RunPackageScript(const std::string &PackageDir, const std::string &Sysroot, bool AllowNonRoot)
{
	return RunLifecycleScript(PackageDir, "INSTALL", Sysroot, AllowNonRoot, {});
}

}
