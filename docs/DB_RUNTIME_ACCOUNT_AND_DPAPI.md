# 数据库运行账号与 DPAPI 实施说明

## 1. 文档状态

本文描述 2026-10-17 当前落地的单机安全模型，用于交给另一个 agent 复核。

当前决策：

1. MySQL 结构迁移、建表和 6 个缓存触发器全部由管理员完成。
2. `QT_Face.exe` 使用纯 DML 账号 `face_app@localhost`。
3. 程序运行路径不执行任何 DDL。
4. 程序运行账号没有 `TRIGGER` 或 `SET_USER_ID`。
5. MySQL 口令使用 Windows DPAPI CurrentUser 范围加密后写入 `config.ini`。

工作区已初始化 Git 仓库并提交初始基线（见根目录 `.gitignore`，`config.ini` 已被排除）；复核可就近查看 `git diff` 与文件内容。

## 2. 角色与权限边界

| 角色 | 用途 | 权限 |
|---|---|---|
| MySQL 管理员/root | 建库、迁移、触发器维护 | DDL、TRIGGER 等 |
| `face_app@localhost` | QT_Face 运行时 | 纯 DML |
| 当前 Windows 用户 | 启动程序、解密 DPAPI | 可读取当前用户 DPAPI 密文 |

### 2.1 当前 `face_app` 授权

实际 `SHOW GRANTS FOR 'face_app'@'localhost'`：

```text
GRANT USAGE ON *.* TO `face_app`@`localhost`
GRANT SELECT ON `face_db`.`face_cache_revision` TO `face_app`@`localhost`
GRANT SELECT ON `face_db`.`face_employee` TO `face_app`@`localhost`
GRANT SELECT, INSERT, DELETE ON `face_db`.`face_feature` TO `face_app`@`localhost`
GRANT SELECT, INSERT ON `face_db`.`face_log` TO `face_app`@`localhost`
GRANT SELECT ON `face_db`.`face_schema_meta` TO `face_app`@`localhost`
```

没有授予：

- `CREATE`
- `ALTER`
- `DROP`
- `REFERENCES`
- `TRIGGER`
- `SET_USER_ID`
- 全局管理权限

账号口令由加密随机数生成，明文没有写入本文、源码或配置文件。

## 3. 程序启动时的数据库行为

### 3.1 不执行 DDL

`facedatabase.cpp` 不再包含：

- `CREATE DATABASE`
- `CREATE TABLE`
- `ALTER TABLE`
- 加外键
- 创建触发器

`FaceDatabase::open()` 直接连接已存在的 `face_db`。数据库不存在时返回连接错误，并提示管理员先初始化。

### 3.2 结构版本检查

`verifySchemaReady()` 只执行一次版本读取：

```sql
SELECT schema_version FROM face_schema_meta WHERE id = 1
```

要求：

```text
schema_version >= 1
```

未迁移时拒绝启动，提示：

```text
数据库结构未迁移或版本无效（要求 face_schema_meta.schema_version >= 1）。
程序运行账号只有 DML 权限，不会自动建表或改表。
请由 MySQL 管理员执行 sql\migrate_personnel_source.sql，确认迁移完成后再启动程序。
```

### 3.3 触发器由管理员维护

6 个缓存触发器由管理员在安装或迁移时执行：

```text
sql\migrate_personnel_source.sql
```

程序运行账号看不到也不补建触发器。这样避免给运行账号 `TRIGGER` 或
`SET_USER_ID`，也避免利用自定义 `DEFINER` 获得高权限执行能力。

触发器健康检查由管理员执行只读脚本：

```bat
cmd /c "mysql -u root -p -D face_db < sql\check_cache_triggers.sql"
```

输出中 `state=缺失` 的触发器，需要管理员重新执行迁移脚本来修复。

注意：纯 DML 设计下，如果 6 个触发器被误删，程序自身无法发现。必须在现场维护
流程中定期运行健康检查，或由独立的管理任务负责检查。

## 4. DPAPI 口令保护

### 4.1 当前配置状态

`config.ini` 当前使用：

```ini
[Database]
user=face_app
password=
passwordDpapi=<Base64 DPAPI 密文>
```

源码配置和 Release 配置均已切换。文件中没有 `root`、`1234` 或 MySQL 明文口令。

### 4.2 解密流程

`AppConfig::load()`：

1. 严格 Base64 解码 `passwordDpapi`；
2. 调用 `CryptUnprotectData()`；
3. 使用 `DPAPI CurrentUser` 范围；
4. 解密出的 UTF-8 字符串作为 MySQL 口令；
5. 解密失败时写入 `AppConfig::configError`，并清空口令；
6. `FaceDatabase::open()` 在连接前检查 `configError`，给出明确错误。

DPAPI 返回的中间明文缓冲区在释放前使用 `SecureZeroMemory()`。

### 4.3 DPAPI 设置工具

工具：

```text
tools\set-db-password-dpapi.ps1
```

使用：

```powershell
.\tools\set-db-password-dpapi.ps1 -ConfigPath .\config.ini
```

必须由实际启动 `QT_Face.exe` 的 Windows 用户执行。工具会：

- 安全提示输入口令；
- 使用 UTF-8 字节加密；
- 清空 `password`；
- 写入 `passwordDpapi`；
- 不打印明文；
- 不生成包含旧明文口令的备份。

### 4.4 口令重置

如果 DPAPI 密文因 Windows 用户变化或用户配置损坏而无法解密：

1. 管理员用 root 重置 `face_app` 密码；
2. 用实际运行程序的 Windows 用户重新执行 DPAPI 工具；
3. 重新启动程序。

命令示例：

```sql
ALTER USER 'face_app'@'localhost' IDENTIFIED BY '新的随机高强度密码';
```

### 4.5 DPAPI 安全边界

DPAPI 能防止 `config.ini` 被直接读取或复制到其他电脑/Windows 用户后复用。

它不能防止：

- 以同一 Windows 用户身份运行的恶意程序；
- 管理员读取该 Windows 用户的安全上下文；
- 程序进程内存中已解密的口令。

程序内 `AppConfig::dbPassword` 仍以 `QString` 形式存在，这是 Qt/MySQL 连接接口的
现实限制。

## 5. 新机器部署顺序

1. 管理员执行 `sql\schema.sql` 或 `sql\migrate_personnel_source.sql`；
2. 确认 `face_schema_meta.schema_version = 1`；
3. 确认 6 个缓存触发器存在；
4. 管理员创建 `face_app@localhost` 并授予纯 DML 权限；
5. 部署 Release 目录；
6. 将 `config.ini` 的 `user` 设置为 `face_app`，清空 `password`；
7. 用实际运行用户执行 DPAPI 工具；
8. 启动程序，确认模型和数据库初始化成功；
9. 管理员定期运行 `check_cache_triggers.sql`。

## 6. 当前验证记录

### 6.1 构建

- MSVC x64 Release 构建通过。
- 部署目录中的 exe、tools、sql、docs 已更新。

### 6.2 DPAPI 与数据库连接

独立测试程序加载正式 DPAPI 配置：

```text
OPEN=1
HAS_PASSWORD=1
CONFIG_ERROR=
OPEN_ERROR=
```

说明 `face_app` 纯 DML 账号和 DPAPI 解密链路可用。

### 6.3 纯 DML 账号

已迁移测试库：

- 纯 DML 账号启动成功；
- 没有使用 `TRIGGER` 或 `SET_USER_ID`。

未迁移测试库：

- 启动被拒绝；
- 返回明确的“请管理员执行迁移脚本”错误；
- 没有尝试建表或改表。

### 6.4 DDL 拒绝

以 `face_app` 执行：

```sql
CREATE TABLE face_db.__face_app_ddl_probe(id INT);
```

结果：

```text
ERROR 1142 (42000): CREATE command denied to user 'face_app'@'localhost'
```

### 6.5 正式程序

使用 Release 配置正常启动并保持运行 8 秒，数据库和模型初始化未弹出错误。

### 6.6 清理

- 临时测试库已删除；
- 临时测试 MySQL 账号已删除；
- 正式 `face_db` 的 `schema_version=1`；
- 正式 `face_db` 的 6 个缓存触发器均存在。

## 7. 给复核 agent 的检查清单

1. 检查 `facedatabase.cpp` 不含 `CREATE TABLE`、`ALTER TABLE`、`CREATE DATABASE`。
2. 检查未迁移时只返回明确错误，不执行 DDL。
3. 检查 `face_app` 的 `SHOW GRANTS` 不包含 `TRIGGER`、`SET_USER_ID`、`CREATE`、`ALTER`。
4. 检查源码和 Release 的 `config.ini`：
   - `user=face_app`
   - `password=` 为空
   - `passwordDpapi` 非空
   - 不包含 `root` 或 `1234`
5. 检查 DPAPI 解密失败时是否能在连接 MySQL 前返回明确错误。
6. 检查管理员迁移脚本能建立 6 个触发器。
7. 检查 `check_cache_triggers.sql` 能输出每个触发器“存在/缺失”。
8. 确认文档不再要求运行账号使用 `TRIGGER` 或 `SET_USER_ID`。

## 8. 已知限制

- 纯 DML 运行账号无法自行发现或修复缺失的触发器，必须依赖管理员维护。
- DPAPI 不能防御同一 Windows 用户下的内存读取或恶意程序。
- DPAPI CurrentUser 密文不能在另一个 Windows 用户下直接使用。
- `config.ini` 被 `.gitignore` 排除，DPAPI 密文与本机 Windows 用户绑定，不入版本库。
