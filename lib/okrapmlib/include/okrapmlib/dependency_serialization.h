#pragma once

#include "okrapmlib/dependency.h"

#include <string>
#include <vector>

namespace okrapm {

// DependencySerialization: 依赖列表的可嵌入文本格式
//
// 为什么单独一个文件，而不是直接改 Object::serialize：
//
// object.h 里 serialize / deserialize 只有声明，格式没有文档。把新字段插进
// 一个未知格式里，风险是已装系统的 system.db 读不出来。所以这里先把新字段的
// 序列化做成一个**自包含的块**，满足三条：
//
//   1. 块只由自己前缀开头的行组成，不依赖上下文。
//   2. 空列表序列化成**空串**，所以旧对象（没有依赖）输出不变。
//   3. 块里有版本号，以后加字段不影响老解析器。
//
// 接进 Object 时，把 SerializeDependencies 的结果拼到 Object::serialize 的
// 输出后面，把 ParseDependencies 作用在 deserialize 的输入上即可。具体拼接
// 位置要看到 object.cpp 的现有实现之后再定，见 verification.md。

/**
 * 格式
 *
 * 块由若干行组成，行之间用 '\n' 分隔。字段之间用制表符分隔。
 *
 *   第一行：  @dependencies<TAB>1
 *   其余行：  @dependency<TAB>Kind<TAB>Source<TAB>Namespace<TAB>Name<TAB>Operator<TAB>Version
 *
 * Kind      DependencyKindText 的输出，例如 requires。
 * Source    DependencySourceText 的输出，例如 manual。
 * Namespace 命名空间，可空。
 * Name      名字。包名、文件路径、soname 都可以。
 * Operator  ComparisonOperatorText 的输出。空字段表示无约束。
 * Version   版本字符串，可空。
 *
 * 字段里的反斜杠、制表符、换行、回车按下表转义：
 *
 *   反斜杠 -> \\    制表符 -> \t    换行 -> \n    回车 -> \r
 *
 * 空列表序列化成空串。解析时遇到不是 @dependencies 或 @dependency 开头的行
 * 直接跳过，所以块可以嵌在更大的文本里。
 *
 * 已知限制：RPM 的 rich deps（(foo if bar) 这类）表达不了，因为没有布尔结构。
 * 遇到这种依赖时，名字会整串存下来，包括空格和括号，能存能读，但不参与求解。
 */

/**
 * SerializeDependencies() - 把依赖列表序列化成可嵌入的文本块。
 * @Items: 依赖列表。
 *
 * Return: 序列化结果。列表为空时返回空串。
 */
std::string SerializeDependencies(const std::vector<Dependency>& Items);

/**
 * ParseDependencies() - 从文本里解析出依赖块。
 * @Text: 待解析的文本。可以包含块以外的内容，那些行会被跳过。
 * @OutItems: 输出依赖列表，解析前会被清空。
 *
 * 判定规则：
 *   没有 @dependencies 行      -> 成功，OutItems 为空。
 *   版本号不是 1               -> 返回 -EINVAL。
 *   行内字段数不足 7           -> 返回 -EINVAL。
 *   行内字段数多于 7           -> 忽略多余字段，便于以后加字段。
 *   Kind 或 Source 不认识      -> 返回 -EINVAL。
 *   Operator 不是合法比较符    -> 返回 -EINVAL。
 *
 * Return: 成功返回 0，格式错误返回 -EINVAL。
 */
int ParseDependencies(const std::string& Text, std::vector<Dependency>& OutItems);

/**
 * LooksLikeDependencyBlock() - 判断文本里是否有依赖块。
 * @Text: 待判定的文本。
 *
 * Return: 出现 @dependencies 或 @dependency 开头的行时返回 true。
 */
bool LooksLikeDependencyBlock(const std::string& Text);

} // namespace okrapm