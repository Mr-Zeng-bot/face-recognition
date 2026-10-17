#ifndef FACEDATABASE_H
#define FACEDATABASE_H

// ============================================================================
// facedatabase.h —— 数据库模块（要求 3 / 8）
// 说明：
//   * 基于 QtSql 连接 MySQL，负责人脸特征入库、1:N 比对、识别日志写入与查询。
//   * 特征以 BLOB（512 * 4 字节 float）存储，比对时加载到内存做余弦相似度计算。
//   * 表结构见工程根目录 sql/schema.sql。
// ============================================================================

#include <QObject>
#include <QVector>
#include <QString>
#include "imagedata.h"

class FaceDatabase : public QObject
{
    Q_OBJECT
public:
    explicit FaceDatabase(QObject *parent = nullptr);
    ~FaceDatabase() override;

    // 打开数据库连接（程序不执行任何 DDL，要求管理员先完成迁移）
    bool open(QString &error);
    void close();
    bool isOpen() const;

    // 人脸注册：人员信息由 MySQL 管理，这里只追加特征。
    bool registerFace(const QString &employeeNo, const QVector<float> &feature,
                      const QString &modelVersion, QString &error);

    // 查询启用人员；不存在或 is_active=0 时返回 false，并给出可读原因。
    bool activeEmployee(const QString &employeeNo, QString &name,
                        QString &department, QString &error) const;

    // 单个工号最多保留的人脸模板数。多模板用于覆盖不同姿态/光照（正面、左右侧脸、
    // 抬头低头等），超过上限时淘汰最旧的一张——模板无节制增长会抬高误识率。
    static constexpr int kMaxTemplatesPerEmployee = 5;

    // 把全部已注册特征加载到内存缓存，用于 1:N 比对
    bool reloadFeatures(QString &error);

    // 外部 MySQL 人员/模板变化时，按版本号刷新内存缓存。
    bool cacheRevision(qint64 &revision, QString &error) const;
    bool syncCacheIfChanged(bool &changed, QString &error);

    // 某工号当前已采集的模板数量（界面用来提示"该工号现有几张模板"）
    int templateCount(const QString &employeeNo, QString &error) const;

    // 1:N 比对：返回相似度最高的记录
    FaceData::MatchResult identify(const QVector<float> &feature, float threshold) const;

    // 写入识别日志（要求 8：识别结果、时间、工号、图片路径）
    bool insertLog(const QString &employeeNo, float similarity,
                   const QString &imagePath, const QString &result,
                   QString &error);

    // 查询最近 N 条日志（用于界面表格）
    QVector<FaceData::LogRow> recentLogs(int limit, QString &error) const;

    // 内存缓存里的模板总数（一人多模板时会大于人数）
    int cachedFeatureCount() const { return static_cast<int>(m_records.size()); }
    // 内存缓存里的不同工号数，即"库内人数"
    int cachedEmployeeCount() const;

private:
    // 校验数据库结构版本；未迁移时明确拒绝启动
    bool verifySchemaReady(QString &error);

    // 特征 <-> BLOB 转换
    static QByteArray featureToBlob(const QVector<float> &feature);
    static QVector<float> blobToFeature(const QByteArray &blob);

    QString m_connectionName;
    QVector<FaceData::FaceRecord> m_records; // 内存中的特征缓存
    qint64 m_cacheRevision = 0;
};

#endif // FACEDATABASE_H
