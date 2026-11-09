#pragma once

#include <string>

namespace Opsis {

struct RuntimeConfig {
	std::string Sysroot;
	std::string DatabaseDirectory;
	bool AllowNonRoot{false};
	std::string ScriptPath;
	std::string BaseDirectory;
	bool PackOnly{false};
	std::string PackageNamespace;
	std::string PackageName;
	std::string PackageVersion;
	std::string PackageDescription;
	std::string PackageArchitecture;
	std::string PackageOutput;
	std::string BuildDirectory;
	std::string EntryName;
	std::string OldVersion;
	std::string NewVersion;
	std::string Reason;
};

struct PackageScene {
	std::string Namespace;
	std::string Name;
	std::string Version;
	std::string OldVersion;
	std::string NewVersion;
	std::string Architecture;
	std::string Reason;
};

const char *HostArchitecture();

RuntimeConfig LoadConfig();

int RunSource(const std::string &Source, const std::string &FileName, const RuntimeConfig &Config);

int RunFile(const std::string &Path, RuntimeConfig Config);

int RunPackageScript(const std::string &PackageDir, const std::string &Sysroot, bool AllowNonRoot);

int RunLifecycleScript(const std::string &PackageDir, const std::string &Entry, const std::string &Sysroot,
	bool AllowNonRoot, const PackageScene &Scene = {});

int RunScriptFile(const std::string &ScriptPath, const std::string &Entry, const std::string &Sysroot,
	bool AllowNonRoot, const PackageScene &Scene = {});

}
