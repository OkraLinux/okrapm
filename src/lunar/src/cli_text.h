#pragma once

#include "okrapmlib/transaction.h"

#include <string>
#include <utility>
#include <vector>

namespace Cli {

void Error(const std::string &Message);

void TaskBegin(const std::string &Verb, const std::string &Subject = "");

void TaskOk(const std::string &Detail);

void TaskFail(const std::string &Detail);

void PrintTable(const std::vector<std::string> &Headers,
	const std::vector<std::vector<std::string>> &Rows, int Indent = 0);

void PrintFields(const std::vector<std::pair<std::string, std::string>> &Rows);

void PrintOperations(const okrapm::Transaction &Txn);

void Help();

}
