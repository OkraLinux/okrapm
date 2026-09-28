#pragma once

#include "okrapmlib/rpm_artifact.h"

#include <string>
#include <vector>

namespace okrapm {

enum class ScriptletPhase
{
	PreTrans,
	PostTrans,
	PreInstall,
	PostInstall,
	PreRemove,
	PostRemove,
	TriggerIn,
	TriggerUn,
};

struct ScriptletDescriptor
{
	ScriptletPhase Phase{ScriptletPhase::PreInstall};
	std::string Interpreter;
	std::string Content;
	std::string PackageName;
	int InstanceCount{1};
};

class RpmScriptBackend
{
public:
	int RunScriptlet(const ScriptletDescriptor& Descriptor,
	                 const std::string& InstallRoot);

	int RunPhase(ScriptletPhase Phase,
	             const std::vector<ScriptletDescriptor>& Scriptlets,
	             const std::string& InstallRoot);

	int RunTransaction(
		const std::vector<RpmScriptlet>& RawScriptlets,
		const std::string& PackageName,
		const std::string& InstallRoot,
		bool IsUpgrade);

private:
	ScriptletPhase ParsePhase(const std::string& RawPhase) const;
	std::string PhaseName(ScriptletPhase Phase) const;
	int RunShell(const std::string& Script, const std::string& Interpreter,
	             const std::string& InstallRoot, int Arg);
};

} // namespace okrapm