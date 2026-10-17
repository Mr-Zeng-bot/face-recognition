#include "facedatabase.h"
#include "appconfig.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlDriver>
#include <QVariant>
#include <QDebug>
#include <QUuid>
#include <QStringList>
#include <cmath>

namespace {

int currentSchemaVersion(QSqlDatabase &db)
{
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT schema_version FROM face_schema_meta WHERE id = 1")) ||
        !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

} // namespace

// ---------------------------------------------------------------------------
// MySQL 数据库模块实现
// ---------------------------------------------------------------------------

FaceDatabase::FaceDatabase(QObject *parent) : QObject(parent)
{
    // 用唯一连接名，避免同名连接被复用导致设置不一致。
    m_connectionName = QStringLiteral("face_conn_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

FaceDatabase::~FaceDatabase()
{
    close();
}

bool FaceDatabase::open(QString &error)
{
    const AppConfig &cfg = AppConfig::instance();

    if (!cfg.configError.isEmpty()) {
        error = cfg.configError;
        return false;
    }

    // 检查 Qt 是否带该数据库驱动（Qt6 默认不含 QMYSQL，需自行编译）
    if (!QSqlDatabase::isDriverAvailable(cfg.dbDriver)) {
        error = QStringLiteral("未找到数据库驱动 %1。Qt6 官方安装包默认不含 QMYSQL，"
                               "请参考 docs 说明编译 qsqlmysql 驱动，或改用 QODBC。")
                    .arg(cfg.dbDriver);
        return false;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(cfg.dbDriver, m_connectionName);
    db.setHostName(cfg.dbHost);
    db.setPort(cfg.dbPort);
    db.setDatabaseName(cfg.dbName);
    db.setUserName(cfg.dbUser);
    db.setPassword(cfg.dbPassword);
    // MySQL 中文需要 utf8mb4
    db.setConnectOptions(QStringLiteral("MYSQL_OPT_CONNECT_TIMEOUT=5"));

    if (!db.open()) {
        // 先取错误文本并释放句柄，否则 removeDatabase 会报 "connection is still in use"
        const QString reason = db.lastError().text();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        error = QStringLiteral("连接 MySQL 失败: ") + reason +
                QStringLiteral("。请确认 face_db 已由管理员完成初始化和迁移。");
        return false;
    }

    // 程序运行账号只有 DML 权限，结构版本由管理员迁移脚本维护。
    if (!verifySchemaReady(error)) {
        close();
        return false;
    }
    // 先把「本次重载对应的版本号」读出来，重载成功后再记录。
    // 反过来（先重载、后读版本）会把重载期间发生的变更误标成已同步，
    // 那条变更就得等下一次修改才会生效。
    qint64 loadedRevision = 0;
    if (!cacheRevision(loadedRevision, error)) {
        close();
        return false;
    }
    if (!reloadFeatures(error)) {
        close();
        return false;
    }
    m_cacheRevision = loadedRevision;

    qDebug() << "[FaceDatabase] 连接成功，缓存特征数:" << m_records.size();
    return true;
}

void FaceDatabase::close()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            if (db.isOpen()) db.close();
        } // 作用域结束，db 析构后才能安全移除连接
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    m_records.clear();
    m_cacheRevision = 0;
}

bool FaceDatabase::isOpen() const
{
    if (!QSqlDatabase::contains(m_connectionName)) return false;
    return QSqlDatabase::database(m_connectionName, false).isOpen();
}

bool FaceDatabase::verifySchemaReady(QString &error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (currentSchemaVersion(db) >= 1)
        return true;

    error = QStringLiteral(
                "数据库结构未迁移或版本无效（要求 face_schema_meta.schema_version >= 1）。"
                "程序运行账号只有 DML 权限，不会自动建表或改表。"
                "请由 MySQL 管理员执行 sql\\migrate_personnel_source.sql，"
                "确认迁移完成后再启动程序。");
    return false;
}

QByteArray FaceDatabase::featureToBlob(const QVector<float> &feature)
{
    QByteArray blob;
    blob.resize(feature.size() * static_cast<int>(sizeof(float)));
    float *dst = reinterpret_cast<float *>(blob.data());
    for (int i = 0; i < feature.size(); ++i) dst[i] = feature[i];
    return blob;
}

QVector<float> FaceDatabase::blobToFeature(const QByteArray &blob)
{
    QVector<float> feature;
    const int count = blob.size() / static_cast<int>(sizeof(float));
    feature.resize(count);
    const float *src = reinterpret_cast<const float *>(blob.constData());
    for (int i = 0; i < count; ++i) feature[i] = src[i];
    return feature;
}

bool FaceDatabase::registerFace(const QString &employeeNo,
                                const QVector<float> &feature,
                                const QString &modelVersion, QString &error)
{
    if (!isOpen()) { error = QStringLiteral("数据库未连接"); return false; }
    if (employeeNo.isEmpty() || feature.isEmpty()) {
        error = QStringLiteral("工号或特征为空");
        return false;
    }

    // 人员主数据只由 MySQL 管理。注册前必须确认工号存在且处于启用状态，
    // 程序既不创建员工，也不覆盖 MySQL 中的姓名/部门。
    QString name;
    QString department;
    if (!activeEmployee(employeeNo, name, department, error))
        return false;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    // 追加特征 / 裁剪超限模板放进一个事务：中途失败整体回滚。
    if (!db.transaction()) {
        error = QStringLiteral("开启事务失败: ") + db.lastError().text();
        return false;
    }

    // 1) 追加写入本次采集的特征。同一工号允许多张模板（正面/左右侧脸/抬头低头等），
    //    识别时取全部模板里的最高相似度，所以多采几个方向能明显提高识别稳定性。
    QSqlQuery ins(db);
    ins.prepare("INSERT INTO face_feature (employee_no, feature, feature_dim, model_version) "
                "VALUES (:no, :feat, :dim, :ver)");
    ins.addBindValue(employeeNo);
    ins.addBindValue(featureToBlob(feature));
    ins.addBindValue(feature.size());
    ins.addBindValue(modelVersion);
    if (!ins.exec()) {
        error = QStringLiteral("写入特征失败: ") + ins.lastError().text();
        db.rollback();
        return false;
    }

    // 2) 超过上限才淘汰最旧的模板。按自增 id 判新旧比 created_at 精确
    //    （created_at 只到秒，同一秒内注册的多张无法区分先后）。
    QSqlQuery keep(db);
    keep.prepare(QStringLiteral("SELECT id FROM face_feature WHERE employee_no = :no "
                                "ORDER BY id DESC LIMIT %1").arg(kMaxTemplatesPerEmployee));
    keep.addBindValue(employeeNo);
    if (!keep.exec()) {
        error = QStringLiteral("查询已有模板失败: ") + keep.lastError().text();
        db.rollback();
        return false;
    }
    QStringList keepIds;
    while (keep.next()) keepIds.append(keep.value(0).toString());
    if (keepIds.size() == kMaxTemplatesPerEmployee) {
        // id 来自本库查询结果是纯整数，拼进 SQL 无注入风险
        QSqlQuery prune(db);
        prune.prepare(QStringLiteral("DELETE FROM face_feature WHERE employee_no = :no "
                                     "AND id NOT IN (%1)")
                          .arg(keepIds.join(QLatin1Char(','))));
        prune.addBindValue(employeeNo);
        if (!prune.exec()) {
            error = QStringLiteral("淘汰超限模板失败: ") + prune.lastError().text();
            db.rollback();
            return false;
        }
    }

    if (!db.commit()) {
        error = QStringLiteral("提交事务失败: ") + db.lastError().text();
        db.rollback();
        return false;
    }

    // 3) 刷新内存缓存（1:N 比对用的是缓存，刷不上必须让调用方知道）
    //    本事务已提交，后面读到的版本号一定包含刚写入的这张模板；先读版本、
    //    再重载，并把版本记为「重载前那一版」，这样重载期间别人改库产生的
    //    更新版本不会被误判成已同步，下一轮轮询会补上。
    qint64 committedRevision = 0;
    if (!cacheRevision(committedRevision, error)) {
        error = QStringLiteral("特征已入库，但读取缓存版本失败: ") + error;
        return false;
    }
    if (!reloadFeatures(error)) {
        error = QStringLiteral("特征已入库，但刷新特征缓存失败: ") + error;
        return false;
    }
    m_cacheRevision = committedRevision;
    return true;
}

bool FaceDatabase::activeEmployee(const QString &employeeNo, QString &name,
                                  QString &department, QString &error) const
{
    name.clear();
    department.clear();
    if (!isOpen()) { error = QStringLiteral("数据库未连接"); return false; }
    if (employeeNo.trimmed().isEmpty()) {
        error = QStringLiteral("工号不能为空");
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT name, department, is_active FROM face_employee "
        "WHERE employee_no = :no"));
    query.addBindValue(employeeNo);
    if (!query.exec()) {
        error = QStringLiteral("查询人员失败: ") + query.lastError().text();
        return false;
    }
    if (!query.next()) {
        error = QStringLiteral("工号 %1 不在 MySQL face_employee 中，请先维护人员信息")
                    .arg(employeeNo);
        return false;
    }
    if (query.value(2).toInt() != 1) {
        error = QStringLiteral("工号 %1 已停用，不能注册或识别").arg(employeeNo);
        return false;
    }

    name = query.value(0).toString();
    department = query.value(1).toString();
    return true;
}

// 某工号当前已采集的模板数量
int FaceDatabase::templateCount(const QString &employeeNo, QString &error) const
{
    if (!isOpen()) { error = QStringLiteral("数据库未连接"); return 0; }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);
    q.prepare("SELECT COUNT(*) FROM face_feature WHERE employee_no = :no");
    q.addBindValue(employeeNo);
    if (!q.exec() || !q.next()) {
        error = QStringLiteral("查询模板数量失败: ") + q.lastError().text();
        return 0;
    }
    return q.value(0).toInt();
}

// 内存缓存里的不同工号数
int FaceDatabase::cachedEmployeeCount() const
{
    QStringList seen;
    for (const FaceData::FaceRecord &rec : m_records) {
        if (!seen.contains(rec.employeeNo)) seen.append(rec.employeeNo);
    }
    return seen.size();
}

bool FaceDatabase::reloadFeatures(QString &error)
{
    if (!isOpen()) { error = QStringLiteral("数据库未连接"); return false; }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    // 只缓存启用人员。人员信息被外部 MySQL 修改后，由 revision 轮询触发重载。
    QSqlQuery q(db);
    const QString sql =
        "SELECT e.employee_no, e.name, f.feature, f.model_version "
        "FROM face_feature f JOIN face_employee e ON e.employee_no = f.employee_no "
        "WHERE e.is_active = 1";
    if (!q.exec(sql)) {
        error = QStringLiteral("加载特征失败: ") + q.lastError().text();
        return false;
    }
    QVector<FaceData::FaceRecord> records;
    while (q.next()) {
        FaceData::FaceRecord rec;
        rec.employeeNo = q.value(0).toString();
        rec.name = q.value(1).toString();
        rec.feature = blobToFeature(q.value(2).toByteArray());
        rec.modelVersion = q.value(3).toString();
        if (!rec.feature.isEmpty())
            records.push_back(rec);
    }
    m_records.swap(records);
    return true;
}

bool FaceDatabase::cacheRevision(qint64 &revision, QString &error) const
{
    revision = 0;
    if (!isOpen()) { error = QStringLiteral("数据库未连接"); return false; }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT revision FROM face_cache_revision WHERE id = 1")) ||
        !query.next()) {
        error = QStringLiteral("读取人员缓存版本失败: ") + query.lastError().text();
        return false;
    }
    revision = query.value(0).toLongLong();
    return true;
}

bool FaceDatabase::syncCacheIfChanged(bool &changed, QString &error)
{
    changed = false;
    qint64 revision = 0;
    if (!cacheRevision(revision, error))
        return false;
    if (revision == m_cacheRevision)
        return true;

    if (!reloadFeatures(error))
        return false;

    // 只把版本推进到「重载前读到的那一版」。重载期间若又发生变更，其版本号更大，
    // 下一轮轮询会再次刷新，不会被漏掉；重载失败时不动 m_cacheRevision，下轮重试。
    m_cacheRevision = revision;
    changed = true;
    return true;
}

FaceData::MatchResult FaceDatabase::identify(const QVector<float> &feature, float threshold) const
{
    FaceData::MatchResult best;
    if (feature.isEmpty() || m_records.isEmpty()) return best;

    float maxSim = -1.0f;
    const FaceData::FaceRecord *bestRec = nullptr;
    for (const FaceData::FaceRecord &rec : m_records) {
        // 维度不同无法比对，跳过
        if (rec.feature.size() != feature.size()) continue;

        // 两个向量均已 L2 归一化，余弦相似度 = 逐元素点积
        double dot = 0.0;
        for (int i = 0; i < feature.size(); ++i)
            dot += static_cast<double>(feature[i]) * rec.feature[i];

        if (dot > maxSim) {
            maxSim = static_cast<float>(dot);
            bestRec = &rec;
        }
    }

    if (bestRec != nullptr) {
        best.similarity = maxSim;
        best.matched = maxSim >= threshold;   // 相似度达到阈值即认为匹配
        best.employeeNo = bestRec->employeeNo;
        best.name = bestRec->name;
    }
    return best;
}

bool FaceDatabase::insertLog(const QString &employeeNo, float similarity,
                             const QString &imagePath, const QString &result,
                             QString &error)
{
    if (!isOpen()) { error = QStringLiteral("数据库未连接"); return false; }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QSqlQuery q(db);
    // 工号->姓名，便于日志表直接展示
    QString name;
    QSqlQuery nq(db);
    nq.prepare("SELECT name FROM face_employee WHERE employee_no = :no");
    nq.addBindValue(employeeNo);
    if (nq.exec() && nq.next()) name = nq.value(0).toString();

    q.prepare("INSERT INTO face_log (employee_no, name, similarity, result, image_path) "
              "VALUES (:no, :name, :sim, :res, :path)");
    q.addBindValue(employeeNo);
    q.addBindValue(name);
    q.addBindValue(similarity);
    q.addBindValue(result);
    q.addBindValue(imagePath);
    if (!q.exec()) {
        error = QStringLiteral("写日志失败: ") + q.lastError().text();
        return false;
    }
    return true;
}

QVector<FaceData::LogRow> FaceDatabase::recentLogs(int limit, QString &error) const
{
    QVector<FaceData::LogRow> rows;
    if (!isOpen()) { error = QStringLiteral("数据库未连接"); return rows; }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery q(db);
    q.prepare("SELECT id, employee_no, name, similarity, result, image_path, capture_time "
              "FROM face_log ORDER BY id DESC LIMIT :lim");
    q.addBindValue(limit);
    if (!q.exec()) { error = QStringLiteral("查询日志失败: ") + q.lastError().text(); return rows; }
    while (q.next()) {
        FaceData::LogRow row;
        row.id = q.value(0).toLongLong();
        row.employeeNo = q.value(1).toString();
        row.name = q.value(2).toString();
        row.similarity = q.value(3).toFloat();
        row.result = q.value(4).toString();
        row.imagePath = q.value(5).toString();
        row.captureTime = q.value(6).toString();
        rows.push_back(row);
    }
    return rows;
}
