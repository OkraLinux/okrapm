#include "okrapmlib/rpm_script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace okrapm {

ScriptletPhase RpmScriptBackend::ParsePhase(const std::string& RawPhase) const
{
	if (RawPhase == "pre" || RawPhase == "preinstall" || RawPhase == "prein") {
		return ScriptletPhase::PreInstall;
	}
	if (RawPhase == "post" || RawPhase == "postinstall" || RawPhase == "postin") {
		return ScriptletPhase::PostInstall;
	}
	if (RawPhase == "preun" || RawPhase == "preuninstall") {
		return ScriptletPhase::PreRemove;
	}
	if (RawPhase == "postun" || RawPhase == "postuninstall") {
		return ScriptletPhase::PostRemove;
	}
	if (RawPhase == "pretrans") {
		return ScriptletPhase::PreTrans;
	}
	if (RawPhase == "posttrans") {
		return ScriptletPhase::PostTrans;
	}
	if (RawPhase == "triggerin") {
		return ScriptletPhase::TriggerIn;
	}
	if (RawPhase == "triggerun") {
		return ScriptletPhase::TriggerUn;
	}
	return ScriptletPhase::PreInstall;
}

std::string RpmScriptBackend::PhaseName(ScriptletPhase Phase) const
{
	switch (Phase) {
	case ScriptletPhase::PreTrans:    return "pretrans";
	case ScriptletPhase::PostTrans:   return "posttrans";
	case ScriptletPhase::PreInstall:  return "pre";
	case ScriptletPhase::PostInstall: return "post";
	case ScriptletPhase::PreRemove:   return "preun";
	case ScriptletPhase::PostRemove:  return "postun";
	case ScriptletPhase::TriggerIn:   return "triggerin";
	case ScriptletPhase::TriggerUn:  return "triggerun";
	}
	return "unknown";
}

int RpmScriptBackend::RunShell(const std::string& Script,
                                const std::string& Interpreter,
                                const std::string& InstallRoot,
                                int Arg)
{
	if (Script.empty()) {
		return 0;
	}

	std::string Shell = Interpreter;
	if (Shell.empty()) {
		Shell = "/bin/sh";
	}

	if (!InstallRoot.empty() && InstallRoot != "/") {
		std::string ChrootShell = InstallRoot + Shell;
		if (access(ChrootShell.c_str(), X_OK) != 0) {
			Shell = "/bin/sh";
		}
	}

	char TmpFile[] = "/tmp/okra_scriptlet_XXXXXX";
	int Fd = mkstemp(TmpFile);
	if (Fd < 0) {
		return -1;
	}
	write(Fd, Script.c_str(), Script.size());
	close(Fd);
	chmod(TmpFile, 0700);

	std::string Cmd;
	if (!InstallRoot.empty() && InstallRoot != "/") {
		Cmd = "chroot '" + InstallRoot + "' " + Shell + " '" + TmpFile + "' " +
		      std::to_string(Arg);
	} else {
		Cmd = Shell + " '" + TmpFile + "' " + std::to_string(Arg);
	}

	Cmd += " 2>&1";

	FILE* Handle = popen(Cmd.c_str(), "r");
	if (Handle == nullptr) {
		unlink(TmpFile);
		return -1;
	}

	char Buffer[4096];
	while (fread(Buffer, 1, sizeof(Buffer), Handle) > 0) {
	}

	int Status = pclose(Handle);
	unlink(TmpFile);

	return WEXITSTATUS(Status);
}

int RpmScriptBackend::RunScriptlet(const ScriptletDescriptor& Descriptor,
                                   const std::string& InstallRoot)
{
	return RunShell(Descriptor.Content, Descriptor.Interpreter,
	               InstallRoot, Descriptor.InstanceCount);
}

int RpmScriptBackend::RunPhase(ScriptletPhase Phase,
                               const std::vector<ScriptletDescriptor>& Scriptlets,
                               const std::string& InstallRoot)
{
	int Failures = 0;
	for (const auto& Scriptlet : Scriptlets) {
		if (Scriptlet.Phase != Phase) {
			continue;
		}
		int Result = RunScriptlet(Scriptlet, InstallRoot);
		if (Result != 0) {
			fprintf(stderr, "[scriptlet] %s %s failed (exit %d)\n",
			        Scriptlet.PackageName.c_str(),
			        PhaseName(Phase).c_str(), Result);
			Failures++;
		}
	}
	return Failures;
}

int RpmScriptBackend::RunTransaction(
	const std::vector<RpmScriptlet>& RawScriptlets,
	const std::string& PackageName,
	const std::string& InstallRoot,
	bool IsUpgrade)
{
	std::vector<ScriptletDescriptor> Scriptlets;
	Scriptlets.reserve(RawScriptlets.size());

	for (const auto& Raw : RawScriptlets) {
		ScriptletDescriptor Desc;
		Desc.Phase = ParsePhase(Raw.Phase);
		Desc.Interpreter = Raw.Interpreter;
		Desc.Content = Raw.Content;
		Desc.PackageName = PackageName;
		Desc.InstanceCount = IsUpgrade ? 2 : 1;
		Scriptlets.push_back(Desc);
	}

	int Failures = 0;

	Failures += RunPhase(ScriptletPhase::PreTrans, Scriptlets, InstallRoot);

	for (const auto& Scriptlet : Scriptlets) {
		if (Scriptlet.Phase == ScriptletPhase::PreInstall) {
			int Result = RunScriptlet(Scriptlet, InstallRoot);
			if (Result != 0) {
				fprintf(stderr, "[scriptlet] %s pre failed (exit %d)\n",
				        PackageName.c_str(), Result);
				Failures++;
			}
		}
	}

	Failures += RunPhase(ScriptletPhase::PostInstall, Scriptlets, InstallRoot);
	Failures += RunPhase(ScriptletPhase::TriggerIn, Scriptlets, InstallRoot);

	Failures += RunPhase(ScriptletPhase::PostTrans, Scriptlets, InstallRoot);

	return Failures;
}

} // namespace okrapm