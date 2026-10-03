#include "detail.h"

namespace Opsis {
namespace detail {

Interpreter::Interpreter(RuntimeConfig Config, std::string FileName)
	: Config_(std::move(Config)), FileName_(std::move(FileName)), Globals_(nullptr), Current_(&Globals_)
{
	if (!Config_.BaseDirectory.empty()) ScriptDirectory_ = fs::absolute(Config_.BaseDirectory);
	else if (Config_.ScriptPath.empty()) ScriptDirectory_ = fs::current_path();
	else ScriptDirectory_ = fs::absolute(Config_.ScriptPath).parent_path();
	OpsisModule_ = std::make_shared<Module>();

	AddNative(Globals_, *OpsisModule_, "LogInfo", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		LogLine("\033[1;32m", "[OPSIS:INFO]", NeedString(Machine, Args[0], 0), std::cout);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "LogWarn", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		LogLine("\033[1;33m", "[OPSIS:WARN]", NeedString(Machine, Args[0], 0), std::cerr);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "LogError", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		LogLine("\033[1;31m", "[OPSIS:FAIL]", NeedString(Machine, Args[0], 0), std::cerr);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "LogStep", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		LogLine("\033[1;36m", "[OPSIS:STEP]", NeedString(Machine, Args[0], 0), std::cout);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Fail", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		Machine.Fail(0, NeedString(Machine, Args[0], 0), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "CheckUser", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		NativeCheckUser(Machine, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "IsRoot", 0, 0, [](Interpreter &, const std::vector<Value> &) {
		return Value::FromBool(geteuid() == 0);
	});
	AddNative(Globals_, *OpsisModule_, "DiskFreeKb", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		struct statvfs Stat {};
		if (statvfs(Machine.Config().Sysroot.c_str(), &Stat) != 0) return Value::FromInt(-1);
		return Value::FromInt(static_cast<int64_t>(Stat.f_bavail) * static_cast<int64_t>(Stat.f_frsize) / 1024);
	});
	AddNative(Globals_, *OpsisModule_, "CheckDiskSpace", 1, 1,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		int64_t Need = NeedInt(Machine, Args[0], 0);
		if (Need <= 0) return Value::Null();
		struct statvfs Stat {};
		if (statvfs(Machine.Config().Sysroot.c_str(), &Stat) != 0) return Value::Null();
		int64_t Avail = static_cast<int64_t>(Stat.f_bavail) * static_cast<int64_t>(Stat.f_frsize) / 1024;
		if (Avail < Need) {
			Machine.Fail(0, "Insufficient disk space in " + Machine.Config().Sysroot
				+ ": required " + std::to_string(Need) + "KB, available " + std::to_string(Avail) + "KB", true);
		}
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "InstallFile", 2, 4, [](Interpreter &Machine, const std::vector<Value> &Args) {
		NativeInstallFile(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "InstallDirectory", 1, 2,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		NativeInstallDirectory(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "UpdateLdconfig", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		fs::path Root = Machine.Config().Sysroot;
		bool Has = access((Root / "usr/bin/ldconfig").c_str(), X_OK) == 0
			|| access((Root / "sbin/ldconfig").c_str(), X_OK) == 0;
		if (!Has) return Value::Null();
		LogLine("\033[1;36m", "[OPSIS:STEP]", "Updating dynamic linker run-time bindings (ldconfig)...", std::cout);
		if (RunProcess({"chroot", Root.string(), "/usr/bin/ldconfig"}, true) != 0) {
			RunProcess({"ldconfig", "-r", Root.string()}, true);
		}
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "UpdateSystemd", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		fs::path Units = fs::path(Machine.Config().Sysroot) / "run/systemd/system";
		if (!fs::is_directory(Units)) return Value::Null();
		if (std::system("command -v systemctl >/dev/null 2>&1") != 0) return Value::Null();
		LogLine("\033[1;36m", "[OPSIS:STEP]", "Reloading systemd daemon...", std::cout);
		RunProcess({"systemctl", "daemon-reload"}, true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "RecordInstalled", 3, 3,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		NativeRecord(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Sysroot", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		return Value::FromString(Machine.Config().Sysroot);
	});
	AddNative(Globals_, *OpsisModule_, "ScriptDirectory", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		return Value::FromString(Machine.ScriptDirectory().string());
	});
	AddNative(Globals_, *OpsisModule_, "Length", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		if (Args[0].IsString()) return Value::FromInt(static_cast<int64_t>(Args[0].Text().size()));
		if (Args[0].IsArray()) return Value::FromInt(static_cast<int64_t>(Args[0].Items()->size()));
		Machine.Fail(0, "Length needs a string or an array", false);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Contains", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const std::string &Text = NeedString(Machine, Args[0], 0);
		const std::string &Part = NeedString(Machine, Args[1], 0);
		return Value::FromBool(Text.find(Part) != std::string::npos);
	});
	AddNative(Globals_, *OpsisModule_, "StartsWith", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const std::string &Text = NeedString(Machine, Args[0], 0);
		const std::string &Part = NeedString(Machine, Args[1], 0);
		return Value::FromBool(Text.size() >= Part.size() && Text.compare(0, Part.size(), Part) == 0);
	});
	AddNative(Globals_, *OpsisModule_, "EndsWith", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const std::string &Text = NeedString(Machine, Args[0], 0);
		const std::string &Part = NeedString(Machine, Args[1], 0);
		if (Text.size() < Part.size()) return Value::FromBool(false);
		return Value::FromBool(Text.compare(Text.size() - Part.size(), Part.size(), Part) == 0);
	});
	AddNative(Globals_, *OpsisModule_, "Substring", 2, 3, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const std::string &Text = NeedString(Machine, Args[0], 0);
		int64_t Start = NeedInt(Machine, Args[1], 0);
		if (Start < 0 || static_cast<uint64_t>(Start) > Text.size()) {
			Machine.Fail(0, "substring is out of range", false);
		}
		int64_t Rest = static_cast<int64_t>(Text.size()) - Start;
		int64_t Count = Args.size() == 3 ? NeedInt(Machine, Args[2], 0) : Rest;
		if (Count < 0 || Count > Rest) Machine.Fail(0, "substring is out of range", false);
		return Value::FromString(Text.substr(static_cast<size_t>(Start), static_cast<size_t>(Count)));
	});
	AddNative(Globals_, *OpsisModule_, "CompareVersion", 2, 2,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		return Value::FromInt(CompareVersionText(NeedString(Machine, Args[0], 0), NeedString(Machine, Args[1], 0)));
	});
	AddNative(Globals_, *OpsisModule_, "Environment", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		const char *Found = std::getenv(NeedString(Machine, Args[0], 0).c_str());
		return Value::FromString(Found ? Found : "");
	});
	AddNative(Globals_, *OpsisModule_, "IsInstalled", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		std::string Name = NeedString(Machine, Args[0], 0);
		if (!ValidPackageName(Name)) Machine.Fail(0, "invalid package name", true);
		fs::path Version = fs::path(Machine.Config().DatabaseDirectory) / Name / "version";
		return Value::FromBool(fs::is_regular_file(Version));
	});
	AddNative(Globals_, *OpsisModule_, "FileExists", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		fs::file_status Status = fs::symlink_status(Full, Error);
		if (Error) return Value::FromBool(false);
		return Value::FromBool(fs::is_regular_file(Status) || fs::is_symlink(Status));
	});
	AddNative(Globals_, *OpsisModule_, "DirectoryExists", 1, 1,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		return Value::FromBool(fs::is_directory(Full, Error));
	});
	AddNative(Globals_, *OpsisModule_, "ReadFile", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		if (!fs::is_regular_file(Full, Error)) Machine.Fail(0, "file does not exist: " + Full.string(), true);
		uintmax_t Size = fs::file_size(Full, Error);
		if (Error || Size > 1048576) Machine.Fail(0, "file is too large to read", true);
		std::ifstream Input(Full, std::ios::binary);
		if (!Input) Machine.Fail(0, "cannot read " + Full.string(), true);
		std::string Text;
		Text.resize(static_cast<size_t>(Size));
		if (Size > 0) Input.read(&Text[0], static_cast<std::streamsize>(Size));
		if (Size > 0 && !Input) Machine.Fail(0, "cannot read " + Full.string(), true);
		return Value::FromString(Text);
	});
	AddNative(Globals_, *OpsisModule_, "WriteFile", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		WriteText(Machine, NeedString(Machine, Args[0], 0), NeedString(Machine, Args[1], 0), false, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "AppendFile", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		WriteText(Machine, NeedString(Machine, Args[0], 0), NeedString(Machine, Args[1], 0), true, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "MakeDirectory", 1, 2,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		fs::create_directories(Full, Error);
		if (Error) Machine.Fail(0, "cannot create directory " + Full.string(), true);
		if (Args.size() == 2) ApplyMode(Full, NeedString(Machine, Args[1], 0));
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "RemoveFile", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		fs::file_status Status = fs::symlink_status(Full, Error);
		if (Error || (!fs::is_regular_file(Status) && !fs::is_symlink(Status))) {
			Machine.Fail(0, "file does not exist: " + Full.string(), true);
		}
		fs::remove(Full, Error);
		if (Error) Machine.Fail(0, "cannot remove " + Full.string(), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "RemoveDirectory", 1, 1,
		[](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Root = fs::path(Machine.Config().Sysroot).lexically_normal();
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		if (Full == Root) Machine.Fail(0, "refusing to remove the sysroot", true);
		std::error_code Error;
		if (!fs::is_directory(Full, Error)) Machine.Fail(0, "directory does not exist: " + Full.string(), true);
		fs::remove_all(Full, Error);
		if (Error) Machine.Fail(0, "cannot remove " + Full.string(), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "MovePath", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path From = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		fs::path To = JoinInside(Machine, NeedString(Machine, Args[1], 0), 0);
		std::error_code Error;
		if (!fs::exists(fs::symlink_status(From, Error))) {
			Machine.Fail(0, "path does not exist: " + From.string(), true);
		}
		fs::create_directories(To.parent_path(), Error);
		if (Error) Machine.Fail(0, "cannot create directory " + To.parent_path().string(), true);
		fs::rename(From, To, Error);
		if (Error) Machine.Fail(0, "cannot move " + From.string(), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Symlink", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		std::string Target = NeedString(Machine, Args[0], 0);
		fs::path Link = JoinInside(Machine, NeedString(Machine, Args[1], 0), 0);
		fs::path TargetPath(Target);
		if (Target.empty()) Machine.Fail(0, "empty symlink target", true);
		if (!TargetPath.is_absolute()) {
			fs::path Root = fs::path(Machine.Config().Sysroot).lexically_normal();
			fs::path Resolved = (Link.parent_path() / TargetPath).lexically_normal();
			fs::path Back = Resolved.lexically_relative(Root);
			if (Back.empty() || *Back.begin() == "..") Machine.Fail(0, "symlink target escapes the sysroot", true);
		}
		std::error_code Error;
		fs::create_directories(Link.parent_path(), Error);
		if (Error) Machine.Fail(0, "cannot create directory " + Link.parent_path().string(), true);
		if (fs::exists(fs::symlink_status(Link, Error))) fs::remove(Link, Error);
		fs::create_symlink(Target, Link, Error);
		if (Error) Machine.Fail(0, "cannot create symlink " + Link.string(), true);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "SetMode", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		fs::path Full = JoinInside(Machine, NeedString(Machine, Args[0], 0), 0);
		std::error_code Error;
		if (!fs::exists(fs::symlink_status(Full, Error))) {
			Machine.Fail(0, "path does not exist: " + Full.string(), true);
		}
		ApplyMode(Full, NeedString(Machine, Args[1], 0));
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "Run", 1, 16, [](Interpreter &Machine, const std::vector<Value> &Args) {
		std::vector<std::string> Argv;
		for (const Value &Arg : Args) Argv.push_back(NeedString(Machine, Arg, 0));
		if (Argv[0].empty()) Machine.Fail(0, "empty program", true);
		int Status = RunProcess(Argv, false);
		if (Status < 0) Machine.Fail(0, "cannot run " + Argv[0], true);
		return Value::FromInt(Status);
	});
	AddNative(Globals_, *OpsisModule_, "UpdateDesktopDatabase", 0, 0,
		[](Interpreter &Machine, const std::vector<Value> &) {
		RunHostTool(Machine, "update-desktop-database", "usr/share/applications",
			"Updating desktop application database...");
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "UpdateMimeDatabase", 0, 0,
		[](Interpreter &Machine, const std::vector<Value> &) {
		RunHostTool(Machine, "update-mime-database", "usr/share/mime", "Updating MIME database...");
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "UpdateIconCache", 0, 0, [](Interpreter &Machine, const std::vector<Value> &) {
		RunHostTool(Machine, "gtk-update-icon-cache", "usr/share/icons/hicolor", "Updating icon cache...");
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "BeginPackage", 0, 4, [](Interpreter &Machine, const std::vector<Value> &Args) {
		BeginDirectPackage(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "PackFile", 2, 3, [](Interpreter &Machine, const std::vector<Value> &Args) {
		PackOneFile(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "PackDirectory", 2, 2, [](Interpreter &Machine, const std::vector<Value> &Args) {
		PackOneDirectory(Machine, Args, 0);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "AddDependency", 1, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		RequireOpen(Machine, 0);
		std::string Name = NeedString(Machine, Args[0], 0);
		if (!PackageToken(Name)) Machine.Fail(0, "invalid dependency name", true);
		PackageSession.Dependencies.push_back(Name);
		return Value::Null();
	});
	AddNative(Globals_, *OpsisModule_, "FinishPackage", 0, 1, [](Interpreter &Machine, const std::vector<Value> &Args) {
		FinishDirectPackage(Machine, Args, 0);
		return Value::Null();
	});
	Globals_.Define("Opsis", Value::FromModule(OpsisModule_));
	BindContext();
}

Interpreter::~Interpreter()
{
	ResetDirectPackage();
}

std::vector<StmtPtr> ParseSource(const std::string &Source, const std::string &FileName);

std::string CanonicalEntry(const std::string &Name)
{
	if (Name == "MAIN" || Name == "Main") return "MAIN";
	if (Name == "INSTALL" || Name == "Install") return "INSTALL";
	if (Name == "UPDATE" || Name == "Update" || Name == "Upgrade") return "UPDATE";
	if (Name == "REMOVE" || Name == "Remove") return "REMOVE";
	return Name;
}

int Interpreter::EntryRankFor(const std::string &MethodName, const std::string &ClassName) const
{
	std::string Canon = CanonicalEntry(MethodName);
	if (!Config_.EntryName.empty()) return Canon == CanonicalEntry(Config_.EntryName) ? 0 : 100;
	if (Canon == "INSTALL") return ClassName.empty() ? 1 : 3;
	if (Canon == "MAIN") return ClassName.empty() ? 2 : 4;
	return 100;
}

bool Interpreter::DerivesFrom(const std::string &Type, const std::string &Base) const
{
	if (Type.empty() || Base.empty()) return false;
	std::string At = Type;
	for (int Hop = 0; Hop < 32; ++Hop) {
		auto Found = BaseOf_.find(At);
		if (Found == BaseOf_.end() || Found->second.empty()) return false;
		if (Found->second == Base) return true;
		At = Found->second;
	}
	return false;
}

void Interpreter::RegisterEntry(const std::shared_ptr<Function> &Fn, int Rank)
{
	if (Rank >= 100) return;
	auto MethodOf = [](const Function &Item) {
		auto Dot = Item.Name.rfind('.');
		return Dot == std::string::npos ? Item.Name : Item.Name.substr(Dot + 1);
	};
	if (!Entry_ || Rank < EntryRank_) {
		Entry_ = Fn;
		EntryRank_ = Rank;
		return;
	}
	if (Rank != EntryRank_) return;
	if (CanonicalEntry(MethodOf(*Fn)) != CanonicalEntry(MethodOf(*Entry_))) return;
	if (Fn->Owner == Entry_->Owner || DerivesFrom(Fn->Owner, Entry_->Owner)) Entry_ = Fn;
}

void Interpreter::DefineFunction(const FunctionStmt *Node, const std::string &ClassName)
{
	auto Fn = std::make_shared<Function>();
	Fn->Owner = ClassName;
	Fn->Name = ClassName.empty() ? Node->Name() : ClassName + "." + Node->Name();
	Fn->ReturnType = Node->ReturnType();
	Fn->Params = Node->Params();
	Fn->Body = Node->Body();
	RegisterEntry(Fn, EntryRankFor(Node->Name(), ClassName));
	if (ClassName.empty()) Globals_.Define(Node->Name(), Value::FromFunction(Fn));
}

void Interpreter::DefineClass(const ClassStmt *Node)
{
	if (Node->Name() == Node->Base()) Fail(Node->Line(), "a class cannot inherit from itself", false);
	std::shared_ptr<Module> BaseMod;
	if (!Node->Base().empty()) {
		try {
			Value BaseValue = Globals_.Get(Node->Base());
			if (!BaseValue.IsModule()) Fail(Node->Line(), "base class is not a class: " + Node->Base(), false);
			BaseMod = BaseValue.ModulePtr();
		} catch (const RuntimeError &) {
			Fail(Node->Line(), "base class is not defined: " + Node->Base(), false);
		}
	}
	BaseOf_[Node->Name()] = Node->Base();
	auto Mod = std::make_shared<Module>();
	if (BaseMod) {
		for (const auto &Member : BaseMod->Members) {
			bool Hidden = false;
			for (const ClassField &Field : Node->Fields()) {
				if (Field.Name == Member.first) Hidden = true;
			}
			for (const auto &Method : Node->Methods()) {
				if (Method->Name() == Member.first) Hidden = true;
			}
			if (Hidden) continue;
			if (Member.second.IsFunction() && Member.second.FunctionPtr()) {
				auto Copy = std::make_shared<Function>(*Member.second.FunctionPtr());
				Copy->Owner = Node->Name();
				Copy->Name = Node->Name() + "." + Member.first;
				Mod->Members[Member.first] = Value::FromFunction(Copy);
				RegisterEntry(Copy, EntryRankFor(Member.first, Node->Name()));
			} else if (!Member.second.IsFunction() && !Member.second.IsNative()) {
				Mod->Members[Member.first] = Member.second;
			}
		}
	}
	{
		Environment Local(&Globals_);
		Local.BindFields(Mod.get());
		EnvironmentGuard Guard(*this, &Local);
		for (const ClassField &Field : Node->Fields()) {
			Value Item = Value::Null();
			if (Field.Init) Item = Field.Init->Evaluate(*this);
			else if (Field.Type == "int") Item = Value::FromInt(0);
			else if (Field.Type == "bool") Item = Value::FromBool(false);
			else if (Field.Type == "string") Item = Value::FromString("");
			else if (IsArrayTypeName(Field.Type)) Item = Value::FromArray(ArrayElementName(Field.Type), {});
			else Fail(Node->Line(), "field requires an initializer: " + Field.Name, false);
			if (!TypeOk(Field.Type, Item)) {
				Fail(Node->Line(), "field initializer type does not match " + Field.Type, false);
			}
			Mod->Members[Field.Name] = Item;
		}
	}
	for (const auto &Method : Node->Methods()) {
		bool BaseHas = BaseMod && BaseMod->Members.count(Method->Name())
			&& BaseMod->Members[Method->Name()].IsFunction();
		if (Method->Override() && !BaseHas) {
			Fail(Method->Body() ? Method->Line() : Node->Line(),
				Method->Name() + " does not override a base method", false);
		}
		if (!Method->Override() && BaseHas) {
			Fail(Method->Body() ? Method->Line() : Node->Line(),
				Method->Name() + " hides " + Node->Base() + "." + Method->Name() + ", use override", false);
		}
		auto Fn = std::make_shared<Function>();
		Fn->Owner = Node->Name();
		Fn->Name = Node->Name() + "." + Method->Name();
		Fn->ReturnType = Method->ReturnType();
		Fn->Params = Method->Params();
		Fn->Body = Method->Body();
		Mod->Members[Method->Name()] = Value::FromFunction(Fn);
		RegisterEntry(Fn, EntryRankFor(Method->Name(), Node->Name()));
	}
	Globals_.Define(Node->Name(), Value::FromModule(Mod));
}

void Interpreter::ImportSource(const std::string &Path, int Line)
{
	fs::path Full = fs::absolute(ResolveSource(*this, Path)).lexically_normal();
	if (!Imported_.insert(Full.string()).second) return;
	if (!fs::is_regular_file(Full)) Fail(Line, "cannot import " + Path, false);
	std::ifstream Input(Full);
	if (!Input) Fail(Line, "cannot import " + Path, false);
	std::stringstream Buffer;
	Buffer << Input.rdbuf();
	OwnedSources_.push_back(ParseSource(Buffer.str(), Full.string()));
	for (const auto &Item : OwnedSources_.back()) {
		if (auto *Imported = dynamic_cast<const ImportStmt *>(Item.get())) Imported->Execute(*this);
		else if (auto *Fn = dynamic_cast<const FunctionStmt *>(Item.get())) DefineFunction(Fn, "");
		else if (auto *Cls = dynamic_cast<const ClassStmt *>(Item.get())) DefineClass(Cls);
		else Item->Execute(*this);
	}
}

void ImportStmt::Execute(Interpreter &Machine) const
{
	Machine.ImportSource(Path_, Line());
}

void Interpreter::BindContext()
{
	auto Context = std::make_shared<Module>();
	auto Put = [&](const char *Name, const std::string &Text) {
		Context->Members[Name] = Value::FromString(Text);
	};
	std::string Entry = CanonicalEntry(Config_.EntryName);
	std::string NewVersion = Config_.NewVersion.empty() ? Config_.PackageVersion : Config_.NewVersion;
	Put("Entry", Entry);
	Put("Sysroot", Config_.Sysroot);
	Put("Architecture", Config_.PackageArchitecture.empty() ? HostArchitecture() : Config_.PackageArchitecture);
	Put("Namespace", Config_.PackageNamespace);
	Put("Name", Config_.PackageName);
	Put("Version", Config_.PackageVersion);
	Put("OldVersion", Config_.OldVersion);
	Put("NewVersion", NewVersion);
	Put("Reason", Config_.Reason.empty() ? Entry : Config_.Reason);
	std::string OldPackage;
	std::string NewPackage;
	if (!Config_.PackageNamespace.empty() && !Config_.PackageName.empty()) {
		if (!Config_.OldVersion.empty()) {
			OldPackage = Config_.PackageNamespace + "." + Config_.PackageName + "@" + Config_.OldVersion;
		}
		if (!NewVersion.empty()) {
			NewPackage = Config_.PackageNamespace + "." + Config_.PackageName + "@" + NewVersion;
		}
	}
	Put("OldPackage", OldPackage);
	Put("NewPackage", NewPackage);
	Context_ = Context;
	Globals_.Define("Context", Value::FromModule(Context));

	auto Runtime = std::make_shared<Module>();
	auto AddDefault = [&](const char *Name) {
		Runtime->Members[Name] = Value::FromNative(Name, 0, 1,
			[](Interpreter &, const std::vector<Value> &) { return Value::FromInt(0); });
	};
	AddDefault("DefaultInstall");
	AddDefault("DefaultUpdate");
	AddDefault("DefaultRemove");
	Globals_.Define("Runtime", Value::FromModule(Runtime));
}

void Interpreter::Run(const std::vector<StmtPtr> &Items)
{
	for (const auto &Item : Items) {
		if (auto *Imported = dynamic_cast<const ImportStmt *>(Item.get())) Imported->Execute(*this);
	}
	std::vector<const Stmt *> Top;
	for (const auto &Item : Items) {
		if (dynamic_cast<const ImportStmt *>(Item.get())) continue;
		if (auto *Fn = dynamic_cast<const FunctionStmt *>(Item.get())) DefineFunction(Fn, "");
		else if (auto *Cls = dynamic_cast<const ClassStmt *>(Item.get())) DefineClass(Cls);
		else Top.push_back(Item.get());
	}
	if (!Top.empty()) {
		for (const Stmt *Item : Top) Item->Execute(*this);
		return;
	}
	if (!Entry_) {
		if (!Config_.EntryName.empty()) return;
		Fail(1, "no MAIN or INSTALL method", false);
	}
	std::vector<Value> Args;
	if (!Entry_->Params.empty()) {
		if (Entry_->Params.size() != 1 || !Context_) Fail(1, Entry_->Name + " entry takes one context", false);
		Args.push_back(Value::FromModule(Context_));
	}
	Value Result = CallFunction(*Entry_, Args, 1);
	if (Result.IsInt() && Result.Int() != 0) {
		Fail(1, "entry returned " + std::to_string(Result.Int()), true);
	}
}

std::vector<StmtPtr> ParseSource(const std::string &Source, const std::string &FileName)
{
	Parser Tokens(Source, FileName);
	return Tokens.Parse();
}

bool ClassHasEntry(const ClassStmt *Node, const std::string &Canon,
	const std::unordered_map<std::string, const ClassStmt *> &ByName, int Depth)
{
	if (!Node || Depth > 16) return false;
	for (const auto &Method : Node->Methods()) {
		if (CanonicalEntry(Method->Name()) == Canon) return true;
	}
	auto Found = ByName.find(Node->Base());
	if (Found == ByName.end()) return false;
	return ClassHasEntry(Found->second, Canon, ByName, Depth + 1);
}

bool DefinesEntry(const std::vector<StmtPtr> &Items, const std::string &Entry)
{
	std::string Canon = CanonicalEntry(Entry);
	std::unordered_map<std::string, const ClassStmt *> ByName;
	for (const auto &Item : Items) {
		if (auto *Cls = dynamic_cast<const ClassStmt *>(Item.get())) ByName[Cls->Name()] = Cls;
	}
	for (const auto &Item : Items) {
		if (auto *Fn = dynamic_cast<const FunctionStmt *>(Item.get())) {
			if (CanonicalEntry(Fn->Name()) == Canon) return true;
		} else if (auto *Cls = dynamic_cast<const ClassStmt *>(Item.get())) {
			if (ClassHasEntry(Cls, Canon, ByName, 0)) return true;
		}
	}
	return false;
}

fs::path FindEntryProgram(const fs::path &Root, const std::string &Entry)
{
	std::string Canon = CanonicalEntry(Entry);
	std::vector<std::pair<std::string, bool>> Names = {
		{"package.opsis", false},
		{"lifecycle.opsis", false}
	};
	auto AddScene = [&](const std::string &Stem) {
		Names.emplace_back(Stem + ".opsis", true);
	};
	if (Canon == "INSTALL") AddScene("install");
	else if (Canon == "UPDATE") {
		AddScene("update");
		AddScene("upgrade");
	} else if (Canon == "REMOVE") AddScene("remove");
	else if (Canon == "MAIN") AddScene("main");
	else {
		std::string Lower = Canon;
		for (char &Ch : Lower) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
		AddScene(Lower);
	}
	for (const auto &Name : Names) {
		for (const fs::path &Dir : {Root / "scripts", Root}) {
			fs::path Path = Dir / Name.first;
			if (!fs::is_regular_file(Path)) continue;
			try {
				std::ifstream Input(Path);
				if (!Input) continue;
				std::stringstream Buffer;
				Buffer << Input.rdbuf();
				auto Items = ParseSource(Buffer.str(), Path.string());
				if (DefinesEntry(Items, Entry) || (Name.second && !Items.empty())) return Path;
			} catch (const RuntimeError &) {
				return Path;
			}
		}
	}
	return {};
}

}
}
