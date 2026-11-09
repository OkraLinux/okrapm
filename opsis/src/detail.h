#pragma once

#include "opsis/interpreter.h"

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace Opsis {

const char *HostArchitecture();

namespace detail {

struct RuntimeError : std::runtime_error {
	int Code;
	bool Printed;

	RuntimeError(int InCode, const std::string &Message, bool InPrinted)
		: std::runtime_error(Message), Code(InCode), Printed(InPrinted) {}
};

class Interpreter;

enum class ValueKind {
	Null,
	Int,
	Bool,
	String,
	Array,
	Native,
	Function,
	Module
};

struct Function;
struct Module;

class Value {
public:
	Value() = default;

	static Value Null() { return Value(); }
	static Value FromInt(int64_t Number);
	static Value FromBool(bool Flag);
	static Value FromString(std::string Text);
	static Value FromArray(std::string ElementType, std::vector<Value> Items);
	static Value FromNative(std::string Name, int MinArgs, int MaxArgs,
		std::function<Value(Interpreter &, const std::vector<Value> &)> Fn);
	static Value FromFunction(std::shared_ptr<Function> Fn);
	static Value FromModule(std::shared_ptr<Module> Mod);

	ValueKind Kind() const { return Kind_; }
	bool IsNull() const { return Kind_ == ValueKind::Null; }
	bool IsInt() const { return Kind_ == ValueKind::Int; }
	bool IsBool() const { return Kind_ == ValueKind::Bool; }
	bool IsString() const { return Kind_ == ValueKind::String; }
	bool IsArray() const { return Kind_ == ValueKind::Array; }
	bool IsNative() const { return Kind_ == ValueKind::Native; }
	bool IsFunction() const { return Kind_ == ValueKind::Function; }
	bool IsModule() const { return Kind_ == ValueKind::Module; }

	int64_t Int() const { return Int_; }
	bool Bool() const { return Bool_; }
	const std::string &Text() const { return Text_; }
	const std::string &ElementType() const { return ElementType_; }
	const std::shared_ptr<std::vector<Value>> &Items() const { return Items_; }
	const std::string &NativeName() const { return NativeName_; }
	int NativeMin() const { return NativeMin_; }
	int NativeMax() const { return NativeMax_; }
	const std::function<Value(Interpreter &, const std::vector<Value> &)> &NativeFn() const { return NativeFn_; }
	const std::shared_ptr<Function> &FunctionPtr() const { return Function_; }
	const std::shared_ptr<Module> &ModulePtr() const { return Module_; }

	std::string ToString() const;

private:
	ValueKind Kind_{ValueKind::Null};
	int64_t Int_{0};
	bool Bool_{false};
	std::string Text_;
	std::string ElementType_;
	std::shared_ptr<std::vector<Value>> Items_;
	std::string NativeName_;
	int NativeMin_{0};
	int NativeMax_{0};
	std::function<Value(Interpreter &, const std::vector<Value> &)> NativeFn_;
	std::shared_ptr<Function> Function_;
	std::shared_ptr<Module> Module_;
};

struct Module {
	std::unordered_map<std::string, Value> Members;
};

struct ReturnSignal {
	Value Result;
};

struct BreakSignal {};

struct ContinueSignal {};

struct ThrowSignal {
	std::string TypeName;
	std::string Message;
};

struct GotoCaseSignal {
	bool IsDefault{false};
	Value Label;
};

class Environment {
public:
	explicit Environment(Environment *Parent) : Parent_(Parent) {}

	void Define(const std::string &Name, Value Item)
	{
		if (Values_.count(Name)) {
			throw RuntimeError(2, "name already defined: " + Name, false);
		}
		Values_[Name] = std::move(Item);
	}

	void Assign(const std::string &Name, Value Item)
	{
		if (Values_.count(Name)) {
			Values_[Name] = std::move(Item);
			return;
		}
		if (WritableField(Name)) {
			Fields_->Members[Name] = std::move(Item);
			return;
		}
		if (Parent_) {
			Parent_->Assign(Name, std::move(Item));
			return;
		}
		throw RuntimeError(2, "name is not defined: " + Name, false);
	}

	Value Get(const std::string &Name) const
	{
		auto Found = Values_.find(Name);
		if (Found != Values_.end()) return Found->second;
		if (Fields_) {
			auto Field = Fields_->Members.find(Name);
			if (Field != Fields_->Members.end()) return Field->second;
		}
		if (Parent_) return Parent_->Get(Name);
		throw RuntimeError(2, "name is not defined: " + Name, false);
	}

	bool Has(const std::string &Name) const
	{
		return Values_.count(Name) != 0;
	}

	void BindFields(Module *Fields) { Fields_ = Fields; }

private:
	bool WritableField(const std::string &Name) const
	{
		if (!Fields_) return false;
		auto Found = Fields_->Members.find(Name);
		if (Found == Fields_->Members.end()) return false;
		return !Found->second.IsFunction() && !Found->second.IsNative();
	}

	Environment *Parent_;
	Module *Fields_{nullptr};
	std::unordered_map<std::string, Value> Values_;
};

class Expr {
public:
	explicit Expr(int Line) : Line_(Line) {}
	virtual ~Expr() = default;
	virtual Value Evaluate(Interpreter &Machine) const = 0;
	int Line() const { return Line_; }

private:
	int Line_;
};

class Stmt {
public:
	explicit Stmt(int Line) : Line_(Line) {}
	virtual ~Stmt() = default;
	virtual void Execute(Interpreter &Machine) const = 0;
	int Line() const { return Line_; }

private:
	int Line_;
};

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

struct Function {
	std::string Name;
	std::string Owner;
	std::string ReturnType;
	std::vector<std::pair<std::string, std::string>> Params;
	const Stmt *Body{nullptr};
};

enum class TokenKind {
	End, Ident, Int, String,
	KwTrue, KwFalse, KwNull, KwVoid, KwInt, KwString, KwBool, KwVar,
	KwIf, KwElse, KwWhile, KwFor, KwForeach, KwIn, KwBreak, KwContinue, KwReturn,
	KwClass, KwPublic, KwStatic, KwPrivate, KwImport,
	KwAbstract, KwVirtual, KwOverride, KwThrow, KwNew,
	KwSwitch, KwCase, KwDefault, KwGoto,
	LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Semicolon, Dot, Colon, Question,
	Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
	PlusPlus, MinusMinus,
	Plus, Minus, Star, Slash, Percent, Bang,
	EqualEqual, BangEqual, Less, Greater, LessEqual, GreaterEqual,
	AmpAmp, PipePipe
};

struct Token {
	TokenKind Kind{TokenKind::End};
	std::string Text;
	int64_t IntValue{0};
	int Line{1};
};

class LiteralExpr : public Expr {
public:
	LiteralExpr(int Line, Value Item) : Expr(Line), Item_(std::move(Item)) {}
	Value Evaluate(Interpreter &) const override { return Item_; }

private:
	Value Item_;
};

class VariableExpr : public Expr {
public:
	VariableExpr(int Line, std::string Name) : Expr(Line), Name_(std::move(Name)) {}
	Value Evaluate(Interpreter &Machine) const override;
	const std::string &Name() const { return Name_; }

private:
	std::string Name_;
};

class AssignExpr : public Expr {
public:
	AssignExpr(int Line, std::string Name, ExprPtr ValueExpr)
		: Expr(Line), Name_(std::move(Name)), ValueExpr_(std::move(ValueExpr)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	std::string Name_;
	ExprPtr ValueExpr_;
};

class UnaryExpr : public Expr {
public:
	UnaryExpr(int Line, TokenKind Op, ExprPtr Right)
		: Expr(Line), Op_(Op), Right_(std::move(Right)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	TokenKind Op_;
	ExprPtr Right_;
};

class BinaryExpr : public Expr {
public:
	BinaryExpr(int Line, ExprPtr Left, TokenKind Op, ExprPtr Right)
		: Expr(Line), Left_(std::move(Left)), Op_(Op), Right_(std::move(Right)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Left_;
	TokenKind Op_;
	ExprPtr Right_;
};

class CallExpr : public Expr {
public:
	CallExpr(int Line, ExprPtr Callee, std::vector<ExprPtr> Args)
		: Expr(Line), Callee_(std::move(Callee)), Args_(std::move(Args)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Callee_;
	std::vector<ExprPtr> Args_;
};

class GetExpr : public Expr {
public:
	GetExpr(int Line, ExprPtr Object, std::string Name)
		: Expr(Line), Object_(std::move(Object)), Name_(std::move(Name)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Object_;
	std::string Name_;
};

class ArrayExpr : public Expr {
public:
	ArrayExpr(int Line, std::vector<ExprPtr> Items)
		: Expr(Line), Items_(std::move(Items)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	std::vector<ExprPtr> Items_;
};

class IndexExpr : public Expr {
public:
	IndexExpr(int Line, ExprPtr Object, ExprPtr Index)
		: Expr(Line), Object_(std::move(Object)), Index_(std::move(Index)) {}
	Value Evaluate(Interpreter &Machine) const override;
	std::pair<ExprPtr, ExprPtr> Release()
	{
		return {std::move(Object_), std::move(Index_)};
	}

private:
	ExprPtr Object_;
	ExprPtr Index_;
};

class CompoundAssignExpr : public Expr {
public:
	CompoundAssignExpr(int Line, std::string Name, TokenKind Op, ExprPtr ValueExpr)
		: Expr(Line), Name_(std::move(Name)), Op_(Op), ValueExpr_(std::move(ValueExpr)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	std::string Name_;
	TokenKind Op_;
	ExprPtr ValueExpr_;
};

class IndexAssignExpr : public Expr {
public:
	IndexAssignExpr(int Line, ExprPtr Object, ExprPtr Index, TokenKind Op, ExprPtr ValueExpr)
		: Expr(Line), Object_(std::move(Object)), Index_(std::move(Index)), Op_(Op),
		ValueExpr_(std::move(ValueExpr)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Object_;
	ExprPtr Index_;
	TokenKind Op_;
	ExprPtr ValueExpr_;
};

class UpdateExpr : public Expr {
public:
	UpdateExpr(int Line, std::string Name, bool Prefix, int Delta)
		: Expr(Line), Name_(std::move(Name)), Prefix_(Prefix), Delta_(Delta) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	std::string Name_;
	bool Prefix_;
	int Delta_;
};

class TernaryExpr : public Expr {
public:
	TernaryExpr(int Line, ExprPtr Cond, ExprPtr ThenArm, ExprPtr ElseArm)
		: Expr(Line), Cond_(std::move(Cond)), ThenArm_(std::move(ThenArm)), ElseArm_(std::move(ElseArm)) {}
	Value Evaluate(Interpreter &Machine) const override;

private:
	ExprPtr Cond_;
	ExprPtr ThenArm_;
	ExprPtr ElseArm_;
};

class ExprStmt : public Stmt {
public:
	ExprStmt(int Line, ExprPtr Item) : Stmt(Line), Item_(std::move(Item)) {}
	void Execute(Interpreter &Machine) const override;

private:
	ExprPtr Item_;
};

class VarStmt : public Stmt {
public:
	VarStmt(int Line, std::string TypeName, std::string Name, ExprPtr Init)
		: Stmt(Line), TypeName_(std::move(TypeName)), Name_(std::move(Name)), Init_(std::move(Init)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::string TypeName_;
	std::string Name_;
	ExprPtr Init_;
};

class BlockStmt : public Stmt {
public:
	BlockStmt(int Line, std::vector<StmtPtr> Items) : Stmt(Line), Items_(std::move(Items)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::vector<StmtPtr> Items_;
};

class IfStmt : public Stmt {
public:
	IfStmt(int Line, ExprPtr Cond, StmtPtr ThenArm, StmtPtr ElseArm)
		: Stmt(Line), Cond_(std::move(Cond)), ThenArm_(std::move(ThenArm)), ElseArm_(std::move(ElseArm)) {}
	void Execute(Interpreter &Machine) const override;

private:
	ExprPtr Cond_;
	StmtPtr ThenArm_;
	StmtPtr ElseArm_;
};

class WhileStmt : public Stmt {
public:
	WhileStmt(int Line, ExprPtr Cond, StmtPtr Body)
		: Stmt(Line), Cond_(std::move(Cond)), Body_(std::move(Body)) {}
	void Execute(Interpreter &Machine) const override;

private:
	ExprPtr Cond_;
	StmtPtr Body_;
};

class ForStmt : public Stmt {
public:
	ForStmt(int Line, StmtPtr Init, ExprPtr Cond, ExprPtr Step, StmtPtr Body)
		: Stmt(Line), Init_(std::move(Init)), Cond_(std::move(Cond)), Step_(std::move(Step)), Body_(std::move(Body)) {}
	void Execute(Interpreter &Machine) const override;

private:
	StmtPtr Init_;
	ExprPtr Cond_;
	ExprPtr Step_;
	StmtPtr Body_;
};

class ReturnStmt : public Stmt {
public:
	ReturnStmt(int Line, ExprPtr ValueExpr) : Stmt(Line), ValueExpr_(std::move(ValueExpr)) {}
	void Execute(Interpreter &Machine) const override;

private:
	ExprPtr ValueExpr_;
};

class BreakStmt : public Stmt {
public:
	explicit BreakStmt(int Line) : Stmt(Line) {}
	void Execute(Interpreter &) const override { throw BreakSignal(); }
};

class ContinueStmt : public Stmt {
public:
	explicit ContinueStmt(int Line) : Stmt(Line) {}
	void Execute(Interpreter &) const override { throw ContinueSignal(); }
};

class ForeachStmt : public Stmt {
public:
	ForeachStmt(int Line, std::string TypeName, std::string Name, ExprPtr Collection, StmtPtr Body)
		: Stmt(Line), TypeName_(std::move(TypeName)), Name_(std::move(Name)),
		Collection_(std::move(Collection)), Body_(std::move(Body)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::string TypeName_;
	std::string Name_;
	ExprPtr Collection_;
	StmtPtr Body_;
};

class FunctionStmt : public Stmt {
public:
	FunctionStmt(int Line, std::string Name, std::string ReturnType,
		std::vector<std::pair<std::string, std::string>> Params, StmtPtr Body)
		: Stmt(Line), Name_(std::move(Name)), ReturnType_(std::move(ReturnType)),
		Params_(std::move(Params)), Body_(std::move(Body)) {}
	void Execute(Interpreter &) const override {}
	const std::string &Name() const { return Name_; }
	const std::string &ReturnType() const { return ReturnType_; }
	const std::vector<std::pair<std::string, std::string>> &Params() const { return Params_; }
	const Stmt *Body() const { return Body_.get(); }
	void MarkOverride() { Override_ = true; }
	bool Override() const { return Override_; }

private:
	std::string Name_;
	std::string ReturnType_;
	std::vector<std::pair<std::string, std::string>> Params_;
	StmtPtr Body_;
	bool Override_{false};
};

struct ClassField {
	std::string Type;
	std::string Name;
	ExprPtr Init;
};

class ClassStmt : public Stmt {
public:
	ClassStmt(int Line, std::string Name, std::string Base, std::vector<ClassField> Fields,
		std::vector<std::unique_ptr<FunctionStmt>> Methods)
		: Stmt(Line), Name_(std::move(Name)), Base_(std::move(Base)), Fields_(std::move(Fields)),
		Methods_(std::move(Methods)) {}
	void Execute(Interpreter &) const override {}
	const std::string &Name() const { return Name_; }
	const std::string &Base() const { return Base_; }
	const std::vector<ClassField> &Fields() const { return Fields_; }
	const std::vector<std::unique_ptr<FunctionStmt>> &Methods() const { return Methods_; }

private:
	std::string Name_;
	std::string Base_;
	std::vector<ClassField> Fields_;
	std::vector<std::unique_ptr<FunctionStmt>> Methods_;
};

struct SwitchArm {
	bool IsDefault{false};
	ExprPtr Label;
	std::vector<StmtPtr> Body;
};

class SwitchStmt : public Stmt {
public:
	SwitchStmt(int Line, ExprPtr Disc, std::vector<SwitchArm> Arms)
		: Stmt(Line), Disc_(std::move(Disc)), Arms_(std::move(Arms)) {}
	void Execute(Interpreter &Machine) const override;

private:
	int FindArm(Interpreter &Machine, const Value &Key, bool WantDefault) const;
	ExprPtr Disc_;
	std::vector<SwitchArm> Arms_;
};

class ThrowStmt : public Stmt {
public:
	ThrowStmt(int Line, std::string TypeName, std::vector<ExprPtr> Args)
		: Stmt(Line), TypeName_(std::move(TypeName)), Args_(std::move(Args)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::string TypeName_;
	std::vector<ExprPtr> Args_;
};

class GotoCaseStmt : public Stmt {
public:
	GotoCaseStmt(int Line, bool IsDefault, ExprPtr Label)
		: Stmt(Line), IsDefault_(IsDefault), Label_(std::move(Label)) {}
	void Execute(Interpreter &Machine) const override;

private:
	bool IsDefault_;
	ExprPtr Label_;
};

class ImportStmt : public Stmt {
public:
	ImportStmt(int Line, std::string Path) : Stmt(Line), Path_(std::move(Path)) {}
	void Execute(Interpreter &Machine) const override;

private:
	std::string Path_;
};

class EnvironmentGuard {
public:
	EnvironmentGuard(Interpreter &Machine, Environment *Next);
	~EnvironmentGuard();

private:
	Interpreter &Machine_;
	Environment *Previous_;
};

class Interpreter {
public:
	Interpreter(RuntimeConfig Config, std::string FileName);
	~Interpreter();

	void Run(const std::vector<StmtPtr> &Items);
	void DefineFunction(const FunctionStmt *Node, const std::string &ClassName);
	void DefineClass(const ClassStmt *Node);
	void RegisterEntry(const std::shared_ptr<Function> &Fn, int Rank);
	bool DerivesFrom(const std::string &Type, const std::string &Base) const;
	int EntryRankFor(const std::string &MethodName, const std::string &ClassName) const;
	void ImportSource(const std::string &Path, int Line);
	void BindContext();
	[[noreturn]] void Fail(int Line, const std::string &Message, bool OpsisFormat);
	Value CallFunction(const Function &Fn, const std::vector<Value> &Args, int Line);
	Environment *Current() { return Current_; }
	void SetCurrent(Environment *Next) { Current_ = Next; }
	const RuntimeConfig &Config() const { return Config_; }
	const fs::path &ScriptDirectory() const { return ScriptDirectory_; }

private:
	RuntimeConfig Config_;
	std::string FileName_;
	fs::path ScriptDirectory_;
	Environment Globals_;
	Environment *Current_;
	std::shared_ptr<Function> Entry_;
	int EntryRank_{100};
	std::shared_ptr<Module> OpsisModule_;
	std::shared_ptr<Module> Context_;
	std::unordered_set<std::string> Imported_;
	std::vector<std::vector<StmtPtr>> OwnedSources_;
	std::unordered_map<std::string, std::string> BaseOf_;
};

class Lexer {
public:
	Lexer(std::string Source, std::string FileName)
		: Source_(std::move(Source)), FileName_(std::move(FileName)) {}

	Token Next()
	{
		SkipSpace();
		if (At_ >= Source_.size()) return Make(TokenKind::End, "");
		char Ch = Source_[At_];
		if (std::isalpha(static_cast<unsigned char>(Ch)) || Ch == '_') return Ident();
		if (std::isdigit(static_cast<unsigned char>(Ch))) return Number();
		if (Ch == '"') return String();
		if (Ch == '&' && Peek() == '&') { At_ += 2; return Make(TokenKind::AmpAmp, "&&"); }
		if (Ch == '|' && Peek() == '|') { At_ += 2; return Make(TokenKind::PipePipe, "||"); }
		if (Ch == '=' && Peek() == '=') { At_ += 2; return Make(TokenKind::EqualEqual, "=="); }
		if (Ch == '!' && Peek() == '=') { At_ += 2; return Make(TokenKind::BangEqual, "!="); }
		if (Ch == '<' && Peek() == '=') { At_ += 2; return Make(TokenKind::LessEqual, "<="); }
		if (Ch == '>' && Peek() == '=') { At_ += 2; return Make(TokenKind::GreaterEqual, ">="); }
		if (Ch == '+' && Peek() == '+') { At_ += 2; return Make(TokenKind::PlusPlus, "++"); }
		if (Ch == '-' && Peek() == '-') { At_ += 2; return Make(TokenKind::MinusMinus, "--"); }
		if (Ch == '+' && Peek() == '=') { At_ += 2; return Make(TokenKind::PlusAssign, "+="); }
		if (Ch == '-' && Peek() == '=') { At_ += 2; return Make(TokenKind::MinusAssign, "-="); }
		if (Ch == '*' && Peek() == '=') { At_ += 2; return Make(TokenKind::StarAssign, "*="); }
		if (Ch == '/' && Peek() == '=') { At_ += 2; return Make(TokenKind::SlashAssign, "/="); }
		if (Ch == '%' && Peek() == '=') { At_ += 2; return Make(TokenKind::PercentAssign, "%="); }
		At_++;
		switch (Ch) {
		case '(': return Make(TokenKind::LParen, "(");
		case ')': return Make(TokenKind::RParen, ")");
		case '{': return Make(TokenKind::LBrace, "{");
		case '}': return Make(TokenKind::RBrace, "}");
		case '[': return Make(TokenKind::LBracket, "[");
		case ']': return Make(TokenKind::RBracket, "]");
		case ',': return Make(TokenKind::Comma, ",");
		case ';': return Make(TokenKind::Semicolon, ";");
		case '.': return Make(TokenKind::Dot, ".");
		case ':': return Make(TokenKind::Colon, ":");
		case '?': return Make(TokenKind::Question, "?");
		case '=': return Make(TokenKind::Assign, "=");
		case '+': return Make(TokenKind::Plus, "+");
		case '-': return Make(TokenKind::Minus, "-");
		case '*': return Make(TokenKind::Star, "*");
		case '/': return Make(TokenKind::Slash, "/");
		case '%': return Make(TokenKind::Percent, "%");
		case '!': return Make(TokenKind::Bang, "!");
		case '<': return Make(TokenKind::Less, "<");
		case '>': return Make(TokenKind::Greater, ">");
		default:
			throw RuntimeError(2, Where() + "unexpected character", false);
		}
	}

private:
	Token Make(TokenKind Kind, std::string Text) const
	{
		Token Item;
		Item.Kind = Kind;
		Item.Text = std::move(Text);
		Item.Line = Line_;
		return Item;
	}

	std::string Where() const
	{
		return FileName_ + ":" + std::to_string(Line_) + ": ";
	}

	char Peek() const
	{
		if (At_ + 1 >= Source_.size()) return '\0';
		return Source_[At_ + 1];
	}

	void SkipSpace()
	{
		while (At_ < Source_.size()) {
			char Ch = Source_[At_];
			if (Ch == ' ' || Ch == '\t' || Ch == '\r') {
				At_++;
				continue;
			}
			if (Ch == '\n') {
				At_++;
				Line_++;
				continue;
			}
			if (Ch == '/' && Peek() == '/') {
				while (At_ < Source_.size() && Source_[At_] != '\n') At_++;
				continue;
			}
			if (Ch == '/' && Peek() == '*') {
				At_ += 2;
				while (At_ + 1 < Source_.size() && !(Source_[At_] == '*' && Source_[At_ + 1] == '/')) {
					if (Source_[At_] == '\n') Line_++;
					At_++;
				}
				if (At_ + 1 >= Source_.size()) {
					throw RuntimeError(2, Where() + "unclosed comment", false);
				}
				At_ += 2;
				continue;
			}
			break;
		}
	}

	Token Ident()
	{
		size_t Start = At_;
		while (At_ < Source_.size()) {
			unsigned char Ch = static_cast<unsigned char>(Source_[At_]);
			if (!std::isalnum(Ch) && Source_[At_] != '_') break;
			At_++;
		}
		std::string Text = Source_.substr(Start, At_ - Start);
		static const std::unordered_map<std::string, TokenKind> Keywords = {
			{"true", TokenKind::KwTrue}, {"false", TokenKind::KwFalse},
			{"null", TokenKind::KwNull},
			{"void", TokenKind::KwVoid}, {"int", TokenKind::KwInt},
			{"string", TokenKind::KwString}, {"bool", TokenKind::KwBool},
			{"var", TokenKind::KwVar}, {"if", TokenKind::KwIf},
			{"else", TokenKind::KwElse}, {"while", TokenKind::KwWhile},
			{"for", TokenKind::KwFor}, {"foreach", TokenKind::KwForeach},
			{"in", TokenKind::KwIn}, {"break", TokenKind::KwBreak},
			{"continue", TokenKind::KwContinue}, {"return", TokenKind::KwReturn},
			{"class", TokenKind::KwClass}, {"public", TokenKind::KwPublic},
			{"static", TokenKind::KwStatic}, {"private", TokenKind::KwPrivate},
			{"import", TokenKind::KwImport}, {"abstract", TokenKind::KwAbstract},
			{"virtual", TokenKind::KwVirtual}, {"override", TokenKind::KwOverride},
			{"throw", TokenKind::KwThrow}, {"new", TokenKind::KwNew},
			{"switch", TokenKind::KwSwitch}, {"case", TokenKind::KwCase},
			{"default", TokenKind::KwDefault}, {"goto", TokenKind::KwGoto}
		};
		auto Found = Keywords.find(Text);
		if (Found != Keywords.end()) return Make(Found->second, Text);
		return Make(TokenKind::Ident, Text);
	}

	Token Number()
	{
		size_t Start = At_;
		int Base = 10;
		if (Source_[At_] == '0' && At_ + 1 < Source_.size() && (Source_[At_ + 1] == 'x' || Source_[At_ + 1] == 'X')) {
			Base = 16;
			At_ += 2;
			Start = At_;
			while (At_ < Source_.size() && std::isxdigit(static_cast<unsigned char>(Source_[At_]))) At_++;
		} else {
			while (At_ < Source_.size() && std::isdigit(static_cast<unsigned char>(Source_[At_]))) At_++;
		}
		std::string Text = Source_.substr(Start, At_ - Start);
		if (Text.empty()) throw RuntimeError(2, Where() + "bad number", false);
		try {
			Token Item = Make(TokenKind::Int, Text);
			Item.IntValue = std::stoll(Text, nullptr, Base);
			return Item;
		} catch (...) {
			throw RuntimeError(2, Where() + "integer out of range", false);
		}
	}

	Token String()
	{
		At_++;
		std::string Text;
		while (At_ < Source_.size() && Source_[At_] != '"') {
			if (Source_[At_] == '\n') throw RuntimeError(2, Where() + "unterminated string", false);
			if (Source_[At_] == '\\') {
				At_++;
				if (At_ >= Source_.size()) throw RuntimeError(2, Where() + "unterminated string", false);
				char Esc = Source_[At_++];
				if (Esc == 'n') Text.push_back('\n');
				else if (Esc == 't') Text.push_back('\t');
				else if (Esc == 'r') Text.push_back('\r');
				else if (Esc == '\\' || Esc == '"') Text.push_back(Esc);
				else throw RuntimeError(2, Where() + "unknown string escape", false);
				continue;
			}
			Text.push_back(Source_[At_++]);
		}
		if (At_ >= Source_.size() || Source_[At_] != '"') {
			throw RuntimeError(2, Where() + "unterminated string", false);
		}
		At_++;
		return Make(TokenKind::String, Text);
	}

	std::string Source_;
	std::string FileName_;
	size_t At_{0};
	int Line_{1};
};

class Parser {
public:
	Parser(std::string Source, std::string FileName)
		: Lexer_(std::move(Source), FileName), FileName_(std::move(FileName))
	{
		Advance();
		Advance();
		Advance();
		Advance();
		Advance();
	}

	std::vector<StmtPtr> Parse()
	{
		std::vector<StmtPtr> Items;
		while (!Check(TokenKind::End)) {
			SkipModifiers();
			if (Check(TokenKind::KwClass)) Items.push_back(ParseClass());
			else if (LooksLikeFunction()) Items.push_back(ParseFunction());
			else Items.push_back(ParseStatement());
		}
		return Items;
	}

private:
	void Advance()
	{
		Current_ = Upcoming_;
		Upcoming_ = Third_;
		Third_ = Fourth_;
		Fourth_ = Fifth_;
		Fifth_ = Lexer_.Next();
	}

	bool Check(TokenKind Kind) const { return Current_.Kind == Kind; }

	bool Match(TokenKind Kind)
	{
		if (!Check(Kind)) return false;
		Advance();
		return true;
	}

	void Expect(TokenKind Kind, const std::string &What)
	{
		if (!Match(Kind)) throw RuntimeError(2, Where() + "expected " + What, false);
	}

	std::string Where() const
	{
		return FileName_ + ":" + std::to_string(Current_.Line) + ": ";
	}

	void SkipModifiers()
	{
		while (Check(TokenKind::KwPublic) || Check(TokenKind::KwStatic) || Check(TokenKind::KwPrivate)
			|| Check(TokenKind::KwAbstract)) {
			Advance();
		}
	}

	bool TakeMemberModifiers()
	{
		bool Override = false;
		for (;;) {
			if (Check(TokenKind::KwPublic) || Check(TokenKind::KwStatic) || Check(TokenKind::KwPrivate)
				|| Check(TokenKind::KwVirtual) || Check(TokenKind::KwAbstract)) {
				Advance();
			} else if (Check(TokenKind::KwOverride)) {
				Override = true;
				Advance();
			} else {
				break;
			}
		}
		return Override;
	}

	bool IsType(TokenKind Kind) const
	{
		return Kind == TokenKind::KwVoid || Kind == TokenKind::KwInt || Kind == TokenKind::KwString
			|| Kind == TokenKind::KwBool || Kind == TokenKind::KwVar;
	}

	bool LooksLikeFunction() const
	{
		if (Current_.Kind == TokenKind::Ident) {
			return Upcoming_.Kind == TokenKind::Ident && Third_.Kind == TokenKind::LParen;
		}
		if (!IsType(Current_.Kind)) return false;
		if (Upcoming_.Kind == TokenKind::Ident && Third_.Kind == TokenKind::LParen) return true;
		return Upcoming_.Kind == TokenKind::LBracket && Third_.Kind == TokenKind::RBracket
			&& Fourth_.Kind == TokenKind::Ident && Fifth_.Kind == TokenKind::LParen;
	}

	bool IsAssignOp(TokenKind Kind) const
	{
		return Kind == TokenKind::Assign || Kind == TokenKind::PlusAssign || Kind == TokenKind::MinusAssign
			|| Kind == TokenKind::StarAssign || Kind == TokenKind::SlashAssign || Kind == TokenKind::PercentAssign;
	}

	std::string ParseTypeName(bool AllowVoid, bool AllowVar)
	{
		if (Check(TokenKind::Ident)) {
			std::string Name = Current_.Text;
			Advance();
			return Name;
		}
		if (!IsType(Current_.Kind)) throw RuntimeError(2, Where() + "expected a type", false);
		if (!AllowVoid && Current_.Kind == TokenKind::KwVoid) {
			throw RuntimeError(2, Where() + "expected a type", false);
		}
		if (!AllowVar && Current_.Kind == TokenKind::KwVar) {
			throw RuntimeError(2, Where() + "expected a type", false);
		}
		std::string Name = TypeText(Current_.Kind);
		Advance();
		if (!Match(TokenKind::LBracket)) return Name;
		if (Name == "void" || Name == "var") throw RuntimeError(2, Where() + "this type cannot be an array", false);
		Expect(TokenKind::RBracket, "']'");
		return Name + "[]";
	}

	std::string TypeText(TokenKind Kind) const
	{
		if (Kind == TokenKind::KwVoid) return "void";
		if (Kind == TokenKind::KwInt) return "int";
		if (Kind == TokenKind::KwString) return "string";
		if (Kind == TokenKind::KwBool) return "bool";
		if (Kind == TokenKind::KwVar) return "var";
		return "";
	}

	StmtPtr ParseClass()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwClass, "class");
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected class name", false);
		std::string Name = Current_.Text;
		Advance();
		std::string Base;
		if (Match(TokenKind::Colon)) {
			if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a base class", false);
			Base = Current_.Text;
			Advance();
		}
		Expect(TokenKind::LBrace, "'{'");
		std::vector<ClassField> Fields;
		std::vector<std::unique_ptr<FunctionStmt>> Methods;
		while (!Check(TokenKind::RBrace) && !Check(TokenKind::End)) {
			bool Override = TakeMemberModifiers();
			if (!IsType(Current_.Kind) && Current_.Kind != TokenKind::Ident) {
				throw RuntimeError(2, Where() + "expected a field or method", false);
			}
			if (LooksLikeFunction()) {
				auto Node = ParseFunction();
				auto *Fn = dynamic_cast<FunctionStmt *>(Node.get());
				if (!Fn) throw RuntimeError(2, Where() + "expected a method", false);
				if (Override) Fn->MarkOverride();
				Node.release();
				Methods.emplace_back(Fn);
				continue;
			}
			if (Override) throw RuntimeError(2, Where() + "override applies to a method", false);
			ClassField Field;
			Field.Type = ParseTypeName(false, true);
			if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a field name", false);
			Field.Name = Current_.Text;
			Advance();
			if (Match(TokenKind::Assign)) Field.Init = ParseExpression();
			Expect(TokenKind::Semicolon, "';'");
			Fields.push_back(std::move(Field));
		}
		Expect(TokenKind::RBrace, "'}'");
		return std::make_unique<ClassStmt>(Line, Name, Base, std::move(Fields), std::move(Methods));
	}

	StmtPtr ParseFunction()
	{
		int Line = Current_.Line;
		std::string ReturnType = ParseTypeName(true, true);
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a method name", false);
		std::string Name = Current_.Text;
		Advance();
		Expect(TokenKind::LParen, "'('");
		std::vector<std::pair<std::string, std::string>> Params;
		if (!Check(TokenKind::RParen)) {
			do {
				std::string ParamType = ParseTypeName(false, false);
				if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a parameter name", false);
				std::string ParamName = Current_.Text;
				Advance();
				Params.emplace_back(ParamType, ParamName);
			} while (Match(TokenKind::Comma));
		}
		Expect(TokenKind::RParen, "')'");
		StmtPtr Body = ParseBlock();
		return std::make_unique<FunctionStmt>(Line, Name, ReturnType, std::move(Params), std::move(Body));
	}

	StmtPtr ParseImport()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwImport, "import");
		if (!Check(TokenKind::String)) throw RuntimeError(2, Where() + "expected an import path", false);
		std::string Path = Current_.Text;
		Advance();
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ImportStmt>(Line, Path);
	}

	StmtPtr ParseThrow()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwThrow, "throw");
		Expect(TokenKind::KwNew, "new");
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected an exception type", false);
		std::string TypeName = Current_.Text;
		Advance();
		Expect(TokenKind::LParen, "'('");
		std::vector<ExprPtr> Args;
		if (!Check(TokenKind::RParen)) {
			do {
				Args.push_back(ParseExpression());
			} while (Match(TokenKind::Comma));
		}
		Expect(TokenKind::RParen, "')'");
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ThrowStmt>(Line, TypeName, std::move(Args));
	}

	StmtPtr ParseGotoCase()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwGoto, "goto");
		if (Match(TokenKind::KwDefault)) {
			Expect(TokenKind::Semicolon, "';'");
			return std::make_unique<GotoCaseStmt>(Line, true, nullptr);
		}
		Expect(TokenKind::KwCase, "case");
		ExprPtr Label = ParseExpression();
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<GotoCaseStmt>(Line, false, std::move(Label));
	}

	StmtPtr ParseSwitch()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwSwitch, "switch");
		Expect(TokenKind::LParen, "'('");
		ExprPtr Disc = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		Expect(TokenKind::LBrace, "'{'");
		std::vector<SwitchArm> Arms;
		while (!Check(TokenKind::RBrace) && !Check(TokenKind::End)) {
			SwitchArm Arm;
			if (Match(TokenKind::KwDefault)) {
				Arm.IsDefault = true;
			} else {
				Expect(TokenKind::KwCase, "case");
				Arm.Label = ParseExpression();
			}
			Expect(TokenKind::Colon, "':'");
			while (!Check(TokenKind::RBrace) && !Check(TokenKind::KwCase) && !Check(TokenKind::KwDefault)
				&& !Check(TokenKind::End)) {
				Arm.Body.push_back(ParseStatement());
			}
			Arms.push_back(std::move(Arm));
		}
		Expect(TokenKind::RBrace, "'}'");
		return std::make_unique<SwitchStmt>(Line, std::move(Disc), std::move(Arms));
	}

	StmtPtr ParseStatement()
	{
		SkipModifiers();
		if (Check(TokenKind::KwImport)) return ParseImport();
		if (Check(TokenKind::LBrace)) return ParseBlock();
		if (Check(TokenKind::KwIf)) return ParseIf();
		if (Check(TokenKind::KwWhile)) return ParseWhile();
		if (Check(TokenKind::KwFor)) return ParseFor();
		if (Check(TokenKind::KwForeach)) return ParseForeach();
		if (Check(TokenKind::KwBreak)) return ParseBreak();
		if (Check(TokenKind::KwContinue)) return ParseContinue();
		if (Check(TokenKind::KwReturn)) return ParseReturn();
		if (Check(TokenKind::KwThrow)) return ParseThrow();
		if (Check(TokenKind::KwSwitch)) return ParseSwitch();
		if (Check(TokenKind::KwGoto)) return ParseGotoCase();
		if (IsVarType(Current_.Kind)) return ParseVar();
		int Line = Current_.Line;
		ExprPtr Item = ParseExpression();
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ExprStmt>(Line, std::move(Item));
	}

	bool IsVarType(TokenKind Kind) const
	{
		return Kind == TokenKind::KwInt || Kind == TokenKind::KwString || Kind == TokenKind::KwBool
			|| Kind == TokenKind::KwVar;
	}

	StmtPtr ParseBlock()
	{
		int Line = Current_.Line;
		Expect(TokenKind::LBrace, "'{'");
		std::vector<StmtPtr> Items;
		while (!Check(TokenKind::RBrace) && !Check(TokenKind::End)) {
			Items.push_back(ParseStatement());
		}
		Expect(TokenKind::RBrace, "'}'");
		return std::make_unique<BlockStmt>(Line, std::move(Items));
	}

	StmtPtr ParseIf()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwIf, "if");
		Expect(TokenKind::LParen, "'('");
		ExprPtr Cond = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		StmtPtr ThenArm = ParseStatement();
		StmtPtr ElseArm;
		if (Match(TokenKind::KwElse)) ElseArm = ParseStatement();
		return std::make_unique<IfStmt>(Line, std::move(Cond), std::move(ThenArm), std::move(ElseArm));
	}

	StmtPtr ParseWhile()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwWhile, "while");
		Expect(TokenKind::LParen, "'('");
		ExprPtr Cond = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		return std::make_unique<WhileStmt>(Line, std::move(Cond), ParseStatement());
	}

	StmtPtr ParseFor()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwFor, "for");
		Expect(TokenKind::LParen, "'('");
		StmtPtr Init = ParseVar();
		ExprPtr Cond = ParseExpression();
		Expect(TokenKind::Semicolon, "';'");
		ExprPtr Step = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		return std::make_unique<ForStmt>(Line, std::move(Init), std::move(Cond), std::move(Step), ParseStatement());
	}

	StmtPtr ParseForeach()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwForeach, "foreach");
		Expect(TokenKind::LParen, "'('");
		std::string TypeName = ParseTypeName(false, true);
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a variable name", false);
		std::string Name = Current_.Text;
		Advance();
		Expect(TokenKind::KwIn, "in");
		ExprPtr Collection = ParseExpression();
		Expect(TokenKind::RParen, "')'");
		return std::make_unique<ForeachStmt>(Line, TypeName, Name, std::move(Collection), ParseStatement());
	}

	StmtPtr ParseBreak()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwBreak, "break");
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<BreakStmt>(Line);
	}

	StmtPtr ParseContinue()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwContinue, "continue");
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ContinueStmt>(Line);
	}

	StmtPtr ParseReturn()
	{
		int Line = Current_.Line;
		Expect(TokenKind::KwReturn, "return");
		ExprPtr ValueExpr;
		if (!Check(TokenKind::Semicolon)) ValueExpr = ParseExpression();
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<ReturnStmt>(Line, std::move(ValueExpr));
	}

	StmtPtr ParseVar()
	{
		int Line = Current_.Line;
		if (!IsVarType(Current_.Kind)) throw RuntimeError(2, Where() + "expected a variable type", false);
		std::string TypeName = ParseTypeName(false, true);
		if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a variable name", false);
		std::string Name = Current_.Text;
		Advance();
		ExprPtr Init;
		if (Match(TokenKind::Assign)) Init = ParseExpression();
		else if (TypeName == "var") throw RuntimeError(2, Where() + "var requires an initializer", false);
		Expect(TokenKind::Semicolon, "';'");
		return std::make_unique<VarStmt>(Line, TypeName, Name, std::move(Init));
	}

	ExprPtr ParseExpression() { return ParseAssignment(); }

	ExprPtr ParseAssignment()
	{
		ExprPtr Left = ParseTernary();
		if (!IsAssignOp(Current_.Kind)) return Left;
		TokenKind Op = Current_.Kind;
		int Line = Current_.Line;
		Advance();
		ExprPtr Right = ParseAssignment();
		if (auto *Variable = dynamic_cast<VariableExpr *>(Left.get())) {
			if (Op == TokenKind::Assign) {
				return std::make_unique<AssignExpr>(Line, Variable->Name(), std::move(Right));
			}
			return std::make_unique<CompoundAssignExpr>(Line, Variable->Name(), Op, std::move(Right));
		}
		if (auto *Index = dynamic_cast<IndexExpr *>(Left.get())) {
			auto Parts = Index->Release();
			return std::make_unique<IndexAssignExpr>(Line, std::move(Parts.first), std::move(Parts.second), Op,
				std::move(Right));
		}
		throw RuntimeError(2, Where() + "expected an assignable value", false);
	}

	ExprPtr ParseTernary()
	{
		ExprPtr Cond = ParseOr();
		if (!Match(TokenKind::Question)) return Cond;
		int Line = Cond->Line();
		ExprPtr ThenArm = ParseExpression();
		Expect(TokenKind::Colon, "':'");
		return std::make_unique<TernaryExpr>(Line, std::move(Cond), std::move(ThenArm), ParseTernary());
	}

	ExprPtr ParseOr()
	{
		ExprPtr Left = ParseAnd();
		while (Current_.Kind == TokenKind::PipePipe) {
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), TokenKind::PipePipe, ParseAnd());
		}
		return Left;
	}

	ExprPtr ParseAnd()
	{
		ExprPtr Left = ParseEquality();
		while (Current_.Kind == TokenKind::AmpAmp) {
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), TokenKind::AmpAmp, ParseEquality());
		}
		return Left;
	}

	ExprPtr ParseEquality()
	{
		ExprPtr Left = ParseComparison();
		while (Current_.Kind == TokenKind::EqualEqual || Current_.Kind == TokenKind::BangEqual) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), Op, ParseComparison());
		}
		return Left;
	}

	ExprPtr ParseComparison()
	{
		ExprPtr Left = ParseTerm();
		while (Current_.Kind == TokenKind::Less || Current_.Kind == TokenKind::Greater
			|| Current_.Kind == TokenKind::LessEqual || Current_.Kind == TokenKind::GreaterEqual) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), Op, ParseTerm());
		}
		return Left;
	}

	ExprPtr ParseTerm()
	{
		ExprPtr Left = ParseFactor();
		while (Current_.Kind == TokenKind::Plus || Current_.Kind == TokenKind::Minus) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), Op, ParseFactor());
		}
		return Left;
	}

	ExprPtr ParseFactor()
	{
		ExprPtr Left = ParseUnary();
		while (Current_.Kind == TokenKind::Star || Current_.Kind == TokenKind::Slash
			|| Current_.Kind == TokenKind::Percent) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			Left = std::make_unique<BinaryExpr>(Line, std::move(Left), Op, ParseUnary());
		}
		return Left;
	}

	ExprPtr ParseUnary()
	{
		if (Current_.Kind == TokenKind::PlusPlus || Current_.Kind == TokenKind::MinusMinus) {
			int Delta = Current_.Kind == TokenKind::PlusPlus ? 1 : -1;
			int Line = Current_.Line;
			Advance();
			ExprPtr Right = ParseUnary();
			auto *Variable = dynamic_cast<VariableExpr *>(Right.get());
			if (!Variable) throw RuntimeError(2, Where() + "expected a variable", false);
			return std::make_unique<UpdateExpr>(Line, Variable->Name(), true, Delta);
		}
		if (Current_.Kind == TokenKind::Bang || Current_.Kind == TokenKind::Minus) {
			TokenKind Op = Current_.Kind;
			int Line = Current_.Line;
			Advance();
			return std::make_unique<UnaryExpr>(Line, Op, ParseUnary());
		}
		return ParseCall();
	}

	ExprPtr ParseCall()
	{
		ExprPtr Item = ParsePrimary();
		while (true) {
			if (Match(TokenKind::LParen)) {
				int Line = Item->Line();
				std::vector<ExprPtr> Args;
				if (!Check(TokenKind::RParen)) {
					do {
						Args.push_back(ParseExpression());
					} while (Match(TokenKind::Comma));
				}
				Expect(TokenKind::RParen, "')'");
				Item = std::make_unique<CallExpr>(Line, std::move(Item), std::move(Args));
			} else if (Match(TokenKind::Dot)) {
				int Line = Item->Line();
				if (!Check(TokenKind::Ident)) throw RuntimeError(2, Where() + "expected a member name", false);
				std::string Name = Current_.Text;
				Advance();
				Item = std::make_unique<GetExpr>(Line, std::move(Item), Name);
			} else if (Match(TokenKind::LBracket)) {
				int Line = Item->Line();
				ExprPtr Index = ParseExpression();
				Expect(TokenKind::RBracket, "']'");
				Item = std::make_unique<IndexExpr>(Line, std::move(Item), std::move(Index));
			} else if (Current_.Kind == TokenKind::PlusPlus || Current_.Kind == TokenKind::MinusMinus) {
				auto *Variable = dynamic_cast<VariableExpr *>(Item.get());
				if (!Variable) throw RuntimeError(2, Where() + "expected a variable", false);
				int Delta = Current_.Kind == TokenKind::PlusPlus ? 1 : -1;
				int Line = Current_.Line;
				std::string Name = Variable->Name();
				Advance();
				Item = std::make_unique<UpdateExpr>(Line, Name, false, Delta);
			} else {
				break;
			}
		}
		return Item;
	}

	ExprPtr ParsePrimary()
	{
		int Line = Current_.Line;
		if (Match(TokenKind::KwTrue)) return std::make_unique<LiteralExpr>(Line, Value::FromBool(true));
		if (Match(TokenKind::KwFalse)) return std::make_unique<LiteralExpr>(Line, Value::FromBool(false));
		if (Match(TokenKind::KwNull)) return std::make_unique<LiteralExpr>(Line, Value::Null());
		if (Match(TokenKind::LBrace)) return ParseArray(Line);
		if (Check(TokenKind::Int)) {
			int64_t Number = Current_.IntValue;
			Advance();
			return std::make_unique<LiteralExpr>(Line, Value::FromInt(Number));
		}
		if (Check(TokenKind::String)) {
			std::string Text = Current_.Text;
			Advance();
			return std::make_unique<LiteralExpr>(Line, Value::FromString(Text));
		}
		if (Check(TokenKind::Ident)) {
			std::string Name = Current_.Text;
			Advance();
			return std::make_unique<VariableExpr>(Line, Name);
		}
		if (Match(TokenKind::LParen)) {
			ExprPtr Inner = ParseExpression();
			Expect(TokenKind::RParen, "')'");
			return Inner;
		}
		throw RuntimeError(2, Where() + "expected an expression", false);
	}

	ExprPtr ParseArray(int Line)
	{
		std::vector<ExprPtr> Items;
		if (!Check(TokenKind::RBrace)) {
			do {
				Items.push_back(ParseExpression());
			} while (Match(TokenKind::Comma) && !Check(TokenKind::RBrace));
		}
		Expect(TokenKind::RBrace, "'}'");
		return std::make_unique<ArrayExpr>(Line, std::move(Items));
	}

	Lexer Lexer_;
	std::string FileName_;
	Token Current_;
	Token Upcoming_;
	Token Third_;
	Token Fourth_;
	Token Fifth_;
};

struct DirectPackage {
	bool Open{false};
	std::string Namespace;
	std::string Name;
	std::string Version;
	std::string Description;
	std::string Architecture;
	fs::path Output;
	fs::path Stage;
	std::vector<std::string> Files;
	std::vector<std::string> Dependencies;
	uint64_t InstalledSize{0};
};

extern DirectPackage PackageSession;

int64_t NeedInt(Interpreter &Machine, const Value &Item, int Line);
const std::string &NeedString(Interpreter &Machine, const Value &Item, int Line);
bool NeedBool(Interpreter &Machine, const Value &Item, int Line);
bool IsArrayTypeName(const std::string &TypeName);
std::string ArrayElementName(const std::string &TypeName);
bool TypeOk(const std::string &TypeName, const Value &Item);
Value Compute(Interpreter &Machine, int Line, Value Left, TokenKind Op, Value Right);
TokenKind PlainOp(TokenKind Op);
int64_t VersionPart(Interpreter &Machine, const std::string &Text, const std::string &Name, int Line);
bool SameSwitchKey(Interpreter &Machine, int Line, const Value &Left, const Value &Right);
void LogLine(const char *Color, const char *Tag, const std::string &Message, std::ostream &Out);
fs::path JoinInside(Interpreter &Machine, const std::string &Destination, int Line);
fs::path ResolveSource(const Interpreter &Machine, const std::string &Path);
int RunProcess(const std::vector<std::string> &Args, bool Quiet);
void NativeCheckUser(Interpreter &Machine, int Line);
void NativeInstallFile(Interpreter &Machine, const std::vector<Value> &Args, int Line);
void NativeInstallDirectory(Interpreter &Machine, const std::vector<Value> &Args, int Line);
void NativeRecord(Interpreter &Machine, const std::vector<Value> &Args, int Line);
bool ValidPackageName(const std::string &Name);
void ApplyMode(const fs::path &Path, const std::string &Mode);
void WriteText(Interpreter &Machine, const std::string &Destination, const std::string &Text, bool Append, int Line);
bool AllDigits(const std::string &Text);
int CompareVersionText(const std::string &Left, const std::string &Right);
bool CommandExists(const std::string &Name);
void RunHostTool(Interpreter &Machine, const std::string &Tool, const std::string &Relative, const std::string &Step);
void ResetDirectPackage();
bool PackageToken(const std::string &Text);
void RequireOpen(Interpreter &Machine, int Line);
void CopyPackedFile(Interpreter &Machine, const fs::path &From, const fs::path &Relative, const std::string &Mode, int Line);
int WriteArchive(const fs::path &Stage, const fs::path &Output);
void WriteChecksum(const fs::path &Output);
void BeginDirectPackage(Interpreter &Machine, const std::vector<Value> &Args, int Line);
void PackOneFile(Interpreter &Machine, const std::vector<Value> &Args, int Line);
void PackOneDirectory(Interpreter &Machine, const std::vector<Value> &Args, int Line);
void FinishDirectPackage(Interpreter &Machine, const std::vector<Value> &Args, int Line);
void AddNative(Environment &Globals, Module &Mod, const std::string &Name, int MinArgs, int MaxArgs, const std::function<Value(Interpreter &, const std::vector<Value> &)> &Fn);
std::string CanonicalEntry(const std::string &Name);
std::string MethodOf(const Function &Fn);
bool ClassHasEntry(const ClassStmt *Node, const std::string &Canon, const std::unordered_map<std::string, const ClassStmt *> &ByName, int Depth);
bool DefinesEntry(const std::vector<StmtPtr> &Items, const std::string &Entry);
fs::path FindEntryProgram(const fs::path &Root, const std::string &Entry);
std::vector<StmtPtr> ParseSource(const std::string &Source, const std::string &FileName);

}
}
