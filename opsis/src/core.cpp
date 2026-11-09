#include "detail.h"

namespace Opsis {
namespace detail {

Value Value::FromInt(int64_t Number)
{
	Value Item;
	Item.Kind_ = ValueKind::Int;
	Item.Int_ = Number;
	return Item;
}

Value Value::FromBool(bool Flag)
{
	Value Item;
	Item.Kind_ = ValueKind::Bool;
	Item.Bool_ = Flag;
	return Item;
}

Value Value::FromString(std::string Text)
{
	Value Item;
	Item.Kind_ = ValueKind::String;
	Item.Text_ = std::move(Text);
	return Item;
}

Value Value::FromArray(std::string ElementType, std::vector<Value> Items)
{
	Value Item;
	Item.Kind_ = ValueKind::Array;
	Item.ElementType_ = std::move(ElementType);
	Item.Items_ = std::make_shared<std::vector<Value>>(std::move(Items));
	return Item;
}

Value Value::FromNative(std::string Name, int MinArgs, int MaxArgs,
	std::function<Value(Interpreter &, const std::vector<Value> &)> Fn)
{
	Value Item;
	Item.Kind_ = ValueKind::Native;
	Item.NativeName_ = std::move(Name);
	Item.NativeMin_ = MinArgs;
	Item.NativeMax_ = MaxArgs;
	Item.NativeFn_ = std::move(Fn);
	return Item;
}

Value Value::FromFunction(std::shared_ptr<Function> Fn)
{
	Value Item;
	Item.Kind_ = ValueKind::Function;
	Item.Function_ = std::move(Fn);
	return Item;
}

Value Value::FromModule(std::shared_ptr<Module> Mod)
{
	Value Item;
	Item.Kind_ = ValueKind::Module;
	Item.Module_ = std::move(Mod);
	return Item;
}

std::string Value::ToString() const
{
	switch (Kind_) {
	case ValueKind::Null: return "null";
	case ValueKind::Int: return std::to_string(Int_);
	case ValueKind::Bool: return Bool_ ? "true" : "false";
	case ValueKind::String: return Text_;
	case ValueKind::Array: return "array";
	case ValueKind::Native: return NativeName_;
	case ValueKind::Function: return Function_ ? Function_->Name : "function";
	case ValueKind::Module: return "module";
	}
	return "";
}

EnvironmentGuard::EnvironmentGuard(Interpreter &Machine, Environment *Next)
	: Machine_(Machine), Previous_(Machine.Current())
{
	Machine_.SetCurrent(Next);
}

EnvironmentGuard::~EnvironmentGuard()
{
	Machine_.SetCurrent(Previous_);
}

int64_t NeedInt(Interpreter &Machine, const Value &Item, int Line)
{
	if (!Item.IsInt()) Machine.Fail(Line, "expected int, got " + Item.ToString(), false);
	return Item.Int();
}

const std::string &NeedString(Interpreter &Machine, const Value &Item, int Line)
{
	if (!Item.IsString()) Machine.Fail(Line, "expected string", false);
	return Item.Text();
}

bool NeedBool(Interpreter &Machine, const Value &Item, int Line)
{
	if (!Item.IsBool()) Machine.Fail(Line, "expected bool", false);
	return Item.Bool();
}

bool IsArrayTypeName(const std::string &TypeName)
{
	return TypeName.size() > 2 && TypeName.compare(TypeName.size() - 2, 2, "[]") == 0;
}

std::string ArrayElementName(const std::string &TypeName)
{
	if (!IsArrayTypeName(TypeName)) return "";
	return TypeName.substr(0, TypeName.size() - 2);
}

bool TypeOk(const std::string &TypeName, const Value &Item)
{
	if (TypeName == "var") return true;
	if (TypeName == "int") return Item.IsInt();
	if (TypeName == "string") return Item.IsString();
	if (TypeName == "bool") return Item.IsBool();
	if (TypeName == "void") return Item.IsNull();
	if (IsArrayTypeName(TypeName)) {
		return Item.IsArray() && Item.ElementType() == ArrayElementName(TypeName);
	}
	return Item.IsModule();
}

Value VariableExpr::Evaluate(Interpreter &Machine) const
{
	try {
		return Machine.Current()->Get(Name_);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
}

Value AssignExpr::Evaluate(Interpreter &Machine) const
{
	Value Item = ValueExpr_->Evaluate(Machine);
	try {
		Machine.Current()->Assign(Name_, Item);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	return Item;
}

Value UnaryExpr::Evaluate(Interpreter &Machine) const
{
	Value Item = Right_->Evaluate(Machine);
	if (Op_ == TokenKind::Bang) return Value::FromBool(!NeedBool(Machine, Item, Line()));
	if (Op_ == TokenKind::Minus) return Value::FromInt(-NeedInt(Machine, Item, Line()));
	Machine.Fail(Line(), "unknown unary operator", false);
}

Value BinaryExpr::Evaluate(Interpreter &Machine) const
{
	if (Op_ == TokenKind::PipePipe) {
		Value Left = Left_->Evaluate(Machine);
		if (NeedBool(Machine, Left, Line())) return Left;
		return Value::FromBool(NeedBool(Machine, Right_->Evaluate(Machine), Line()));
	}
	if (Op_ == TokenKind::AmpAmp) {
		Value Left = Left_->Evaluate(Machine);
		if (!NeedBool(Machine, Left, Line())) return Left;
		return Value::FromBool(NeedBool(Machine, Right_->Evaluate(Machine), Line()));
	}
	Value Left = Left_->Evaluate(Machine);
	Value Right = Right_->Evaluate(Machine);
	if (Op_ == TokenKind::Plus && (Left.IsString() || Right.IsString())) {
		return Value::FromString(Left.ToString() + Right.ToString());
	}
	if (Op_ == TokenKind::Plus || Op_ == TokenKind::Minus || Op_ == TokenKind::Star
		|| Op_ == TokenKind::Slash || Op_ == TokenKind::Percent) {
		int64_t A = NeedInt(Machine, Left, Line());
		int64_t B = NeedInt(Machine, Right, Line());
		if ((Op_ == TokenKind::Slash || Op_ == TokenKind::Percent) && B == 0) {
			Machine.Fail(Line(), "division by zero", false);
		}
		if (Op_ == TokenKind::Plus) return Value::FromInt(A + B);
		if (Op_ == TokenKind::Minus) return Value::FromInt(A - B);
		if (Op_ == TokenKind::Star) return Value::FromInt(A * B);
		if (Op_ == TokenKind::Slash) return Value::FromInt(A / B);
		return Value::FromInt(A % B);
	}
	if (Op_ == TokenKind::EqualEqual || Op_ == TokenKind::BangEqual) {
		if (Left.IsNull() || Right.IsNull()) {
			bool Same = Left.IsNull() && Right.IsNull();
			return Value::FromBool(Op_ == TokenKind::EqualEqual ? Same : !Same);
		}
	}
	if (Left.Kind() != Right.Kind()) Machine.Fail(Line(), "cannot compare different types", false);
	if (Left.IsInt()) {
		bool Flag = false;
		if (Op_ == TokenKind::EqualEqual) Flag = Left.Int() == Right.Int();
		else if (Op_ == TokenKind::BangEqual) Flag = Left.Int() != Right.Int();
		else if (Op_ == TokenKind::Less) Flag = Left.Int() < Right.Int();
		else if (Op_ == TokenKind::Greater) Flag = Left.Int() > Right.Int();
		else if (Op_ == TokenKind::LessEqual) Flag = Left.Int() <= Right.Int();
		else if (Op_ == TokenKind::GreaterEqual) Flag = Left.Int() >= Right.Int();
		else Machine.Fail(Line(), "unknown operator", false);
		return Value::FromBool(Flag);
	}
	if (Left.IsString()) {
		int Cmp = Left.Text().compare(Right.Text());
		bool Flag = false;
		if (Op_ == TokenKind::EqualEqual) Flag = Cmp == 0;
		else if (Op_ == TokenKind::BangEqual) Flag = Cmp != 0;
		else if (Op_ == TokenKind::Less) Flag = Cmp < 0;
		else if (Op_ == TokenKind::Greater) Flag = Cmp > 0;
		else if (Op_ == TokenKind::LessEqual) Flag = Cmp <= 0;
		else if (Op_ == TokenKind::GreaterEqual) Flag = Cmp >= 0;
		else Machine.Fail(Line(), "unknown operator", false);
		return Value::FromBool(Flag);
	}
	if (Left.IsBool() && (Op_ == TokenKind::EqualEqual || Op_ == TokenKind::BangEqual)) {
		bool Flag = Left.Bool() == Right.Bool();
		if (Op_ == TokenKind::BangEqual) Flag = !Flag;
		return Value::FromBool(Flag);
	}
	Machine.Fail(Line(), "unsupported operands", false);
}

Value Compute(Interpreter &Machine, int Line, Value Left, TokenKind Op, Value Right)
{
	auto LeftExpr = std::make_unique<LiteralExpr>(Line, std::move(Left));
	auto RightExpr = std::make_unique<LiteralExpr>(Line, std::move(Right));
	BinaryExpr Node(Line, std::move(LeftExpr), Op, std::move(RightExpr));
	return Node.Evaluate(Machine);
}

TokenKind PlainOp(TokenKind Op)
{
	if (Op == TokenKind::PlusAssign) return TokenKind::Plus;
	if (Op == TokenKind::MinusAssign) return TokenKind::Minus;
	if (Op == TokenKind::StarAssign) return TokenKind::Star;
	if (Op == TokenKind::SlashAssign) return TokenKind::Slash;
	if (Op == TokenKind::PercentAssign) return TokenKind::Percent;
	return Op;
}

Value ArrayExpr::Evaluate(Interpreter &Machine) const
{
	std::vector<Value> Items;
	std::string ElementType;
	for (const auto &ItemExpr : Items_) {
		Value Item = ItemExpr->Evaluate(Machine);
		std::string This;
		if (Item.IsInt()) This = "int";
		else if (Item.IsString()) This = "string";
		else if (Item.IsBool()) This = "bool";
		else Machine.Fail(Line(), "array elements must be int, string, or bool", false);
		if (ElementType.empty()) ElementType = This;
		else if (ElementType != This) Machine.Fail(Line(), "array element type mismatch", false);
		Items.push_back(std::move(Item));
	}
	return Value::FromArray(ElementType, std::move(Items));
}

Value IndexExpr::Evaluate(Interpreter &Machine) const
{
	Value Object = Object_->Evaluate(Machine);
	if (!Object.IsArray()) Machine.Fail(Line(), "value is not an array", false);
	int64_t At = NeedInt(Machine, Index_->Evaluate(Machine), Line());
	if (At < 0 || static_cast<uint64_t>(At) >= Object.Items()->size()) {
		Machine.Fail(Line(), "array index out of range", false);
	}
	return (*Object.Items())[static_cast<size_t>(At)];
}

Value CompoundAssignExpr::Evaluate(Interpreter &Machine) const
{
	Value Current;
	try {
		Current = Machine.Current()->Get(Name_);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	Value Next = Compute(Machine, Line(), Current, PlainOp(Op_), ValueExpr_->Evaluate(Machine));
	try {
		Machine.Current()->Assign(Name_, Next);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	return Next;
}

Value IndexAssignExpr::Evaluate(Interpreter &Machine) const
{
	Value Object = Object_->Evaluate(Machine);
	if (!Object.IsArray()) Machine.Fail(Line(), "value is not an array", false);
	int64_t At = NeedInt(Machine, Index_->Evaluate(Machine), Line());
	if (At < 0 || static_cast<uint64_t>(At) >= Object.Items()->size()) {
		Machine.Fail(Line(), "array index out of range", false);
	}
	Value Next = ValueExpr_->Evaluate(Machine);
	if (Op_ != TokenKind::Assign) {
		Next = Compute(Machine, Line(), (*Object.Items())[static_cast<size_t>(At)], PlainOp(Op_), Next);
	}
	if (!Object.ElementType().empty() && !TypeOk(Object.ElementType(), Next)) {
		Machine.Fail(Line(), "array element type mismatch", false);
	}
	(*Object.Items())[static_cast<size_t>(At)] = Next;
	return Next;
}

Value UpdateExpr::Evaluate(Interpreter &Machine) const
{
	Value Current;
	try {
		Current = Machine.Current()->Get(Name_);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	TokenKind Op = Delta_ < 0 ? TokenKind::Minus : TokenKind::Plus;
	Value Next = Compute(Machine, Line(), Current, Op, Value::FromInt(1));
	try {
		Machine.Current()->Assign(Name_, Next);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
	return Prefix_ ? Next : Current;
}

Value TernaryExpr::Evaluate(Interpreter &Machine) const
{
	if (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) return ThenArm_->Evaluate(Machine);
	return ElseArm_->Evaluate(Machine);
}

Value CallExpr::Evaluate(Interpreter &Machine) const
{
	Value Callee = Callee_->Evaluate(Machine);
	std::vector<Value> Args;
	for (const auto &Arg : Args_) Args.push_back(Arg->Evaluate(Machine));
	if (Callee.IsNative()) {
		if (static_cast<int>(Args.size()) < Callee.NativeMin() || static_cast<int>(Args.size()) > Callee.NativeMax()) {
			Machine.Fail(Line(), Callee.NativeName() + " argument count mismatch", false);
		}
		return Callee.NativeFn()(Machine, Args);
	}
	if (Callee.IsFunction()) return Machine.CallFunction(*Callee.FunctionPtr(), Args, Line());
	Machine.Fail(Line(), "value is not callable", false);
}

int64_t VersionPart(Interpreter &Machine, const std::string &Text, const std::string &Name, int Line);

Value GetExpr::Evaluate(Interpreter &Machine) const
{
	Value Object = Object_->Evaluate(Machine);
	if (Name_ == "Length" && Object.IsString()) return Value::FromInt(static_cast<int64_t>(Object.Text().size()));
	if (Name_ == "Length" && Object.IsArray()) {
		return Value::FromInt(static_cast<int64_t>(Object.Items()->size()));
	}
	if (Object.IsString() && (Name_ == "Major" || Name_ == "Minor" || Name_ == "Patch")) {
		return Value::FromInt(VersionPart(Machine, Object.Text(), Name_, Line()));
	}
	if (!Object.IsModule()) Machine.Fail(Line(), "member access needs a class", false);
	auto Found = Object.ModulePtr()->Members.find(Name_);
	if (Found == Object.ModulePtr()->Members.end()) {
		Machine.Fail(Line(), "no member " + Name_, false);
	}
	return Found->second;
}

void ExprStmt::Execute(Interpreter &Machine) const
{
	Item_->Evaluate(Machine);
}

void VarStmt::Execute(Interpreter &Machine) const
{
	Value Item = Value::Null();
	if (Init_) Item = Init_->Evaluate(Machine);
	else if (TypeName_ == "int") Item = Value::FromInt(0);
	else if (TypeName_ == "bool") Item = Value::FromBool(false);
	else if (TypeName_ == "string") Item = Value::FromString("");
	else if (IsArrayTypeName(TypeName_)) Item = Value::FromArray(ArrayElementName(TypeName_), {});
	if (Init_ && Item.IsArray() && Item.ElementType().empty() && IsArrayTypeName(TypeName_)) {
		Item = Value::FromArray(ArrayElementName(TypeName_), *Item.Items());
	}
	if (!TypeOk(TypeName_, Item)) Machine.Fail(Line(), "initializer type does not match " + TypeName_, false);
	try {
		Machine.Current()->Define(Name_, Item);
	} catch (const RuntimeError &Error) {
		Machine.Fail(Line(), Error.what(), false);
	}
}

void BlockStmt::Execute(Interpreter &Machine) const
{
	Environment Local(Machine.Current());
	EnvironmentGuard Guard(Machine, &Local);
	for (const auto &Item : Items_) Item->Execute(Machine);
}

void IfStmt::Execute(Interpreter &Machine) const
{
	if (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) ThenArm_->Execute(Machine);
	else if (ElseArm_) ElseArm_->Execute(Machine);
}

void WhileStmt::Execute(Interpreter &Machine) const
{
	while (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) {
		try {
			Body_->Execute(Machine);
		} catch (const ContinueSignal &) {
		} catch (const BreakSignal &) {
			break;
		}
	}
}

void ForStmt::Execute(Interpreter &Machine) const
{
	Environment Local(Machine.Current());
	EnvironmentGuard Guard(Machine, &Local);
	Init_->Execute(Machine);
	while (NeedBool(Machine, Cond_->Evaluate(Machine), Line())) {
		try {
			Body_->Execute(Machine);
		} catch (const ContinueSignal &) {
		} catch (const BreakSignal &) {
			break;
		}
		if (Step_) Step_->Evaluate(Machine);
	}
}

void ReturnStmt::Execute(Interpreter &Machine) const
{
	Value Item = Value::Null();
	if (ValueExpr_) Item = ValueExpr_->Evaluate(Machine);
	throw ReturnSignal{Item};
}

void ForeachStmt::Execute(Interpreter &Machine) const
{
	Value Collection = Collection_->Evaluate(Machine);
	if (!Collection.IsArray()) Machine.Fail(Line(), "foreach needs an array", false);
	if (TypeName_ != "var" && !Collection.ElementType().empty() && Collection.ElementType() != TypeName_) {
		Machine.Fail(Line(), "foreach element type mismatch", false);
	}
	std::vector<Value> Items = *Collection.Items();
	for (const Value &Item : Items) {
		if (!TypeOk(TypeName_, Item)) Machine.Fail(Line(), "foreach element type mismatch", false);
		Environment Local(Machine.Current());
		EnvironmentGuard Guard(Machine, &Local);
		Local.Define(Name_, Item);
		try {
			Body_->Execute(Machine);
		} catch (const ContinueSignal &) {
		} catch (const BreakSignal &) {
			break;
		}
	}
}

int64_t VersionPart(Interpreter &Machine, const std::string &Text, const std::string &Name, int Line)
{
	std::string Core = Text;
	auto Dash = Core.find('-');
	if (Dash != std::string::npos) Core = Core.substr(0, Dash);
	int64_t Parts[3] = {0, 0, 0};
	int Count = 0;
	std::string Token;
	auto Flush = [&]() {
		if (Token.empty() || Count >= 3) Machine.Fail(Line, "value has no version component", false);
		for (char Ch : Token) {
			if (!std::isdigit(static_cast<unsigned char>(Ch))) {
				Machine.Fail(Line, "value has no version component", false);
			}
		}
		try {
			Parts[Count++] = std::stoll(Token);
		} catch (...) {
			Machine.Fail(Line, "value has no version component", false);
		}
		Token.clear();
	};
	for (size_t Index = 0; Index <= Core.size(); ++Index) {
		if (Index == Core.size() || Core[Index] == '.') Flush();
		else Token.push_back(Core[Index]);
	}
	if (Count == 0) Machine.Fail(Line, "value has no version component", false);
	if (Name == "Major") return Parts[0];
	if (Name == "Minor") return Parts[1];
	return Parts[2];
}

bool SameSwitchKey(Interpreter &Machine, int Line, const Value &Left, const Value &Right)
{
	if (Left.IsInt() && Right.IsInt()) return Left.Int() == Right.Int();
	if (Left.IsString() && Right.IsString()) return Left.Text() == Right.Text();
	Machine.Fail(Line, "switch cases must share a type", false);
	return false;
}

int SwitchStmt::FindArm(Interpreter &Machine, const Value &Key, bool WantDefault) const
{
	int DefaultIndex = -1;
	for (int Index = 0; Index < static_cast<int>(Arms_.size()); ++Index) {
		if (Arms_[static_cast<size_t>(Index)].IsDefault) {
			DefaultIndex = Index;
			continue;
		}
		if (WantDefault) continue;
		Value Label = Arms_[static_cast<size_t>(Index)].Label->Evaluate(Machine);
		if (SameSwitchKey(Machine, Line(), Label, Key)) return Index;
	}
	return WantDefault ? DefaultIndex : -1;
}

void SwitchStmt::Execute(Interpreter &Machine) const
{
	Value Key = Disc_->Evaluate(Machine);
	int Index = FindArm(Machine, Key, false);
	if (Index < 0) Index = FindArm(Machine, Key, true);
	while (Index >= 0 && Index < static_cast<int>(Arms_.size())) {
		const SwitchArm &Arm = Arms_[static_cast<size_t>(Index)];
		if (Arm.Body.empty()) {
			++Index;
			continue;
		}
		try {
			for (const auto &Item : Arm.Body) Item->Execute(Machine);
		} catch (const BreakSignal &) {
			return;
		} catch (const GotoCaseSignal &Jump) {
			Index = Jump.IsDefault ? FindArm(Machine, Key, true) : FindArm(Machine, Jump.Label, false);
			if (Index < 0) Machine.Fail(Line(), "switch has no matching case", false);
			continue;
		}
		Machine.Fail(Line(), "switch case falls through", false);
	}
}

void ThrowStmt::Execute(Interpreter &Machine) const
{
	std::string Message;
	for (const auto &Arg : Args_) {
		if (!Message.empty()) Message += ", ";
		Message += Arg->Evaluate(Machine).ToString();
	}
	throw ThrowSignal{TypeName_, Message};
}

void GotoCaseStmt::Execute(Interpreter &Machine) const
{
	if (IsDefault_) throw GotoCaseSignal{true, Value::Null()};
	throw GotoCaseSignal{false, Label_->Evaluate(Machine)};
}

void Interpreter::Fail(int Line, const std::string &Message, bool OpsisFormat)
{
	if (OpsisFormat) {
		std::cerr << "\033[1;31m[OPSIS:FAIL]\033[0m " << Message << "\n";
		throw RuntimeError(1, Message, true);
	}
	throw RuntimeError(2, FileName_ + ":" + std::to_string(Line) + ": " + Message, false);
}

Value Interpreter::CallFunction(const Function &Fn, const std::vector<Value> &Args, int Line)
{
	if (Args.size() != Fn.Params.size()) Fail(Line, Fn.Name + " argument count mismatch", false);
	Environment Local(&Globals_);
	EnvironmentGuard Guard(*this, &Local);
	if (!Fn.Owner.empty()) {
		try {
			Value Owner = Globals_.Get(Fn.Owner);
			if (Owner.IsModule()) Local.BindFields(Owner.ModulePtr().get());
		} catch (const RuntimeError &) {
		}
	}
	for (size_t Index = 0; Index < Fn.Params.size(); ++Index) {
		if (!TypeOk(Fn.Params[Index].first, Args[Index])) {
			Fail(Line, "parameter type mismatch for " + Fn.Params[Index].second, false);
		}
		Local.Define(Fn.Params[Index].second, Args[Index]);
	}
	try {
		Fn.Body->Execute(*this);
	} catch (const BreakSignal &) {
		Fail(Line, "break is outside a loop", false);
	} catch (const ContinueSignal &) {
		Fail(Line, "continue is outside a loop", false);
	} catch (const ReturnSignal &Signal) {
		if (Fn.ReturnType == "void") {
			if (!Signal.Result.IsNull()) Fail(Line, "void method returned a value", false);
			return Value::Null();
		}
		if (!TypeOk(Fn.ReturnType, Signal.Result)) Fail(Line, "return type mismatch", false);
		return Signal.Result;
	}
	if (Fn.ReturnType != "void") Fail(Line, "missing return", false);
	return Value::Null();
}

}
}
