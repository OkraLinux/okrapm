# okpm v3.0.0 — OkraLinux Package Manager

## 编译

```bash
make
```

或手动编译：

```bash
gcc -Wall -Wextra -O2 -D_GNU_SOURCE -o okpm okpm.c
```

静态链接（可跨机器部署）：

```bash
make static
```

## 安装

```bash
sudo make install    # 安装到 /usr/local/bin/
```

或手动复制：

```bash
sudo cp okpm /usr/local/bin/
```

## 依赖

| 工具 | 必需 | 用途 |
|------|------|------|
| gcc  | 是   | 编译 |
| tar  | 是   | 解压/打包 .okra |
| wget 或 curl | 否* | 远程下载包（不配置源则不需要） |
| sha256sum | 否* | 校验包完整性 |
| zstd  | 否* | zstd 压缩的 .okra 包解压/打包 |
| xz    | 否* | xz 压缩 fallback |

标 * 的工具缺失时 okpm 仍可运行，只是对应功能不可用。

## 快速验证

```bash
make test
# 或
./okpm help
./okpm stats
```

## 构建示例包

```bash
cd template
chmod +x scripts/* files/usr/bin/hello
cd ..
./okpm build template/
# 生成 hello-1.0.0.okra
```

## 安装示例包

```bash
sudo ./okpm install hello-1.0.0.okra
hello
sudo ./okpm list
sudo ./okpm info hello
sudo ./okpm remove hello
```

## 目录结构

```
okpm/
├── okpm.c          源码（~2500行，无注释）
├── Makefile         编译配置
├── okpm.conf        配置文件模板
├── README.md        本文件
├── build.sh         一键编译脚本
└── template/        示例包模板
    ├── meta.yaml
    ├── files/
    │   └── usr/bin/hello
    └── scripts/
        ├── pre-install
        ├── post-install
        ├── pre-remove
        └── post-remove
```

## 命令一览

```
okpm install <pkg>...           安装
okpm remove <pkg>...            卸载
okpm purge <pkg>...             彻底清除所有版本
okpm reinstall <pkg>            重装
okpm upgrade [pkg]              升级
okpm update                     检查更新
okpm rollback                   回滚
okpm autoremove                 清理孤儿包
okpm list [--installed|--upgradable|--repos|--locks|--snapshots|--orphans|--groups]
okpm search <keyword>           搜索
okpm info <pkg>                 包详情
okpm files <pkg>                列出包文件
okpm depends <pkg> [depth]      依赖树
okpm rdepends <pkg> [depth]     反向依赖树
okpm provides <file>            查找文件所属包
okpm add-source <url> [pri] [type] [arch]   添加源
okpm remove-source <url>        移除源
okpm build <dir>               构建 .okra
okpm lock <pkg> [version]       锁定版本
okpm unlock <pkg>               解锁
okpm group add|install|member   包组管理
okpm check [pkg]                完整性检查
okpm verify <pkg>               验证文件
okpm clean                      清理
okpm stats                      统计
okpm history [n]                操作历史
okpm export <file>              导出已装列表
okpm import <file>              导入并安装
okpm download <pkg>...          仅下载
```

## 全局选项

```
--yes / -y          跳过确认
--verbose / -v      详细输出
--no-verify         跳过校验
--no-hooks          跳过钩子
--nodeps            跳过依赖解析
--download-only     仅下载
--force / -f        强制操作
```
