#include "facepipeline.h"
#include "appconfig.h"
#include "imagepreprocess.h"
#include "facealigner.h"
#include "facedatabase.h"

#include <QDir>
#include <QDate>
#include <QDateTime>
#include <QFileInfo>
#include <QDebug>
#include <QTimer>

// ---------------------------------------------------------------------------
// 人脸识别流水线实现
// ---------------------------------------------------------------------------

namespace {
// 生成带时间戳的文件名片段，例如 20261015_142530_123
QString timestampForFile()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
}

// 工号由 MySQL 管理员维护，直接拼进文件名可能含 \ / : * ? " < > | 等 Windows
// 非法字符，甚至含 .. 而越出 captures 目录。这里只保留字母数字与 _ -，
// 其余一律替换成下划线并限长，保证文件名成分一定落在目标目录内。
QString safeFileComponent(const QString &raw)
{
    QString out;
    out.reserve(raw.size());
    for (const QChar &c : raw) {
        const bool keep = c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-');
        out.append(keep ? c : QLatin1Char('_'));
    }
    if (out.size() > 64) out.truncate(64);
    return out;
}

// 同一目标重复写识别日志 / 存抓拍图的最小间隔（毫秒）。
// 流水线约 5 帧/秒处理，不做冷却的话相机前有人时日志与抓拍图会无节制增长。
const qint64 kLogCooldownMs = 3000;

// 识别结论的优先级：数值越大越"重要"。一帧里出现多张人脸时，
// 只把优先级最高的那张呈现到界面状态卡上。
int statusRank(FaceData::RecognitionState::Status s)
{
    switch (s) {
    case FaceData::RecognitionState::Matched:        return 5;
    case FaceData::RecognitionState::NotMatched:     return 4;
    case FaceData::RecognitionState::RecognitionOff: return 3;
    case FaceData::RecognitionState::ExtractFailed:  return 2;
    case FaceData::RecognitionState::LowQuality:     return 1;
    case FaceData::RecognitionState::NoFace:         return 0;
    }
    return 0;
}
} // namespace

FacePipeline::FacePipeline(QObject *parent) : QObject(parent) {}

FacePipeline::~FacePipeline() = default;

bool FacePipeline::init(QString &error)
{
    const AppConfig &cfg = AppConfig::instance();

    // 1) 载入 SCRFD 检测模型
    if (!m_detector.load(cfg.scrfdParam, cfg.scrfdBin, cfg.scrfdInputSize,
                         cfg.detScoreThreshold, cfg.detNmsThreshold,
                         cfg.numThreads, cfg.useVulkan, error)) {
        emit initFinished(false, error);
        return false;
    }
    // 2) 载入 ArcFace 特征模型
    if (!m_extractor.load(cfg.arcfaceParam, cfg.arcfaceBin, cfg.arcfaceFeatureDim,
                          cfg.numThreads, cfg.useVulkan, error)) {
        emit initFinished(false, error);
        return false;
    }

    // 3) 数据库：在当前（工作）线程内创建并连接
    m_db = new FaceDatabase(this);
    if (!m_db->open(error)) {
        emit initFinished(false, error);
        return false;
    }

    m_threshold = cfg.matchThreshold;
    m_elapsed.start();
    m_ready = true;

    // QTimer 在 init() 所在的工作线程内创建，槽函数会与识别任务在同一线程串行执行。
    m_cacheTimer = new QTimer(this);
    m_cacheTimer->setInterval(qMax(250, cfg.dbCacheSyncIntervalMs));
    connect(m_cacheTimer, &QTimer::timeout, this, &FacePipeline::syncExternalChanges);
    m_cacheTimer->start();

    emit logMessage(QStringLiteral("人脸流水线初始化完成，已注册 %1 人 / %2 张模板")
                        .arg(m_db->cachedEmployeeCount())
                        .arg(m_db->cachedFeatureCount()));
    emit initFinished(true, QStringLiteral("人脸识别模块初始化完成"));
    return true;
}

QString FacePipeline::saveCapture(const cv::Mat &faceBgr, const QString &employeeNo,
                                  const QString &subDir)
{
    if (faceBgr.empty()) return {};
    const AppConfig &cfg = AppConfig::instance();

    // 按天分目录，避免单目录文件过多；subDir 用于区分注册图与识别抓拍图
    const QString day = QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
    QDir base(cfg.imageSaveDir);
    if (!subDir.isEmpty()) base = QDir(base.filePath(subDir));
    QDir dir(base.filePath(day));
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) return {};

    const QString tag = safeFileComponent(employeeNo);
    const QString name = QStringLiteral("%1_%2.jpg")
                             .arg(tag.isEmpty() ? QStringLiteral("unknown") : tag,
                                  timestampForFile());
    const QString path = dir.filePath(name);
    const QImage image = ImagePreprocess::fromBgrMat(faceBgr);
    if (image.isNull() || !image.save(path, "JPG", 95)) return {};
    return QDir::toNativeSeparators(path);
}

bool FacePipeline::logAllowed(const QString &key)
{
    const qint64 now = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;
    const auto it = m_lastLogTime.constFind(key);
    if (it != m_lastLogTime.constEnd() && now - it.value() < kLogCooldownMs)
        return false;
    m_lastLogTime.insert(key, now);
    return true;
}

void FacePipeline::processFrame(const QImage &image)
{
    if (!m_ready || image.isNull()) return;

    m_busy = true;
    emit busyChanged(true);

    cv::Mat bgr = ImagePreprocess::toBgrMat(image);
    QStringList lines;
    // 本帧的结构化结论，直接交给界面状态卡展示
    FaceData::RecognitionState state;

    if (!bgr.empty()) {
        // 1) SCRFD 检测
        QString detError;
        QVector<FaceData::FaceBox> faces = m_detector.detect(bgr, detError);
        if (!detError.isEmpty()) emit logMessage(detError);

        if (faces.isEmpty()) {
            lines << QStringLiteral("未检测到人脸");
        }

        // 2) 逐张人脸处理
        for (int i = 0; i < faces.size(); ++i) {
            const FaceData::FaceBox &face = faces[i];

            // 2.1 质量判断：模糊 / 过暗 / 过曝 / 过小直接丢弃
            const FaceData::QualityResult quality =
                ImagePreprocess::evaluateQuality(bgr, face.rect);

            QString label;   // 绘制在框上的英文标签（OpenCV 不支持中文）
            QString info;    // 中文详细信息（显示在界面列表 / 状态栏）
            bool identified = false;   // 是否比对成功，决定画框颜色（绿=识别成功）
            FaceData::RecognitionState cand;   // 本张脸的结论，循环末尾按优先级合并

            if (quality.passed) {
                // 2.2 对齐 + 特征提取
                cv::Mat aligned;
                QVector<float> feature;
                QString err;
                if (FaceAligner::align(bgr, face.landmarks, aligned) &&
                    m_extractor.extract(aligned, feature, err)) {

                    if (m_recognitionEnabled) {
                        // 2.3 1:N 比对
                        const FaceData::MatchResult match =
                            m_db->identify(feature, m_threshold);
                        identified = match.matched;
                        const QString employeeNo = match.matched ? match.employeeNo
                                                                 : QString();

                        if (match.matched) {
                            label = QStringLiteral("%1 %2")
                                        .arg(employeeNo)
                                        .arg(QString::number(match.similarity, 'f', 2));
                            info = QStringLiteral("识别成功: 工号 %1 相似度 %2")
                                       .arg(employeeNo)
                                       .arg(match.similarity, 0, 'f', 3);
                            cand.status = FaceData::RecognitionState::Matched;
                            cand.employeeNo = match.employeeNo;
                            cand.name = match.name;
                            cand.similarity = match.similarity;
                        } else {
                            label = QStringLiteral("UNKNOWN %1")
                                        .arg(QString::number(match.similarity, 'f', 2));
                            info = QStringLiteral("未识别(库中最高相似度 %1)")
                                       .arg(match.similarity, 0, 'f', 3);
                            cand.status = FaceData::RecognitionState::NotMatched;
                            cand.similarity = match.similarity;
                        }
                        // 2.4 写识别日志（要求 8）+ 存抓拍图。
                        // 以「工号 / 未识别」为键冷却，同一目标冷却期内只记一次，
                        // 避免相机前持续有人时以帧率级别写库、写图。
                        const QString logKey = match.matched ? employeeNo
                                                             : QStringLiteral("__unknown__");
                        if (logAllowed(logKey)) {
                            const QString capPath = saveCapture(
                                aligned, match.matched ? employeeNo
                                                       : QStringLiteral("unknown"),
                                QStringLiteral("recognize"));
                            QString dbErr;
                            if (!m_db->insertLog(employeeNo, match.similarity, capPath,
                                                 match.matched ? QStringLiteral("匹配")
                                                               : QStringLiteral("未匹配"),
                                                 dbErr)) {
                                emit logMessage(dbErr);
                            } else {
                                if (match.matched)
                                    emit logMessage(QStringLiteral("日志已入库: %1").arg(employeeNo));
                                emit logWritten();   // 通知界面刷新日志表
                            }
                        }
                    } else {
                        label = QStringLiteral("Face %1").arg(i + 1);
                        info = QStringLiteral("已检测人脸（识别关闭），质量通过");
                        cand.status = FaceData::RecognitionState::RecognitionOff;
                    }
                } else {
                    label = QStringLiteral("ExtractFail");
                    info = QStringLiteral("特征提取失败: %1").arg(err);
                    cand.status = FaceData::RecognitionState::ExtractFailed;
                    cand.detail = err;
                }
            } else {
                // 质量未通过：不识别、不入库
                label = QStringLiteral("LowQuality");
                info = QStringLiteral("质量不合格已丢弃: %1").arg(quality.reason);
                cand.status = FaceData::RecognitionState::LowQuality;
                cand.detail = quality.reason;
            }

            if (!info.isEmpty()) lines << info;

            // 多张人脸时只呈现"信息量最大"的那张（命中 > 未命中 > ... > 未检测到）
            if (statusRank(cand.status) > statusRank(state.status)) state = cand;

            // 2.5 绘制人脸框 + 关键点 + 标签（绿=识别成功，红=未识别/质量不合格）
            QVector<QPointF> kps;
            for (int k = 0; k < 5; ++k) kps.push_back(face.landmarks[k]);
            ImagePreprocess::drawFaceInfo(bgr, face, label, kps, identified);
        }

        // 人脸数供界面提示；多张人脸时 state 已在循环里取优先级最高的那张
        state.faceCount = faces.size();
    }

    // 3) 转回 QImage 交给界面显示
    const QImage display = ImagePreprocess::fromBgrMat(bgr);

    m_busy = false;
    emit busyChanged(false);
    emit frameProcessed(display, lines, state);
}

void FacePipeline::registerFace(const QImage &image, const QString &employeeNo)
{
    if (!m_ready || image.isNull()) {
        emit registerFinished(false, QStringLiteral("流水线未就绪或图像为空"));
        return;
    }
    if (employeeNo.trimmed().isEmpty()) {
        emit registerFinished(false, QStringLiteral("工号不能为空"));
        return;
    }

    QString employeeName;
    QString department;
    QString err;
    if (!m_db->activeEmployee(employeeNo, employeeName, department, err)) {
        emit registerFinished(false, err);
        return;
    }

    cv::Mat bgr = ImagePreprocess::toBgrMat(image);
    QVector<FaceData::FaceBox> faces = m_detector.detect(bgr, err);
    if (faces.isEmpty()) {
        emit registerFinished(false, QStringLiteral("未检测到人脸，注册失败"));
        return;
    }

    // 取面积最大的那张脸作为注册对象
    int bestIdx = 0;
    double bestArea = 0.0;
    for (int i = 0; i < faces.size(); ++i) {
        const double area = faces[i].rect.width() * faces[i].rect.height();
        if (area > bestArea) { bestArea = area; bestIdx = i; }
    }
    const FaceData::FaceBox &face = faces[bestIdx];

    // 注册同样要求质量合格
    const FaceData::QualityResult quality = ImagePreprocess::evaluateQuality(bgr, face.rect);
    if (!quality.passed) {
        emit registerFinished(false, QStringLiteral("人脸质量不合格，注册失败: %1").arg(quality.reason));
        return;
    }

    cv::Mat aligned;
    QVector<float> feature;
    if (!FaceAligner::align(bgr, face.landmarks, aligned) ||
        !m_extractor.extract(aligned, feature, err)) {
        emit registerFinished(false, QStringLiteral("特征提取失败: %1").arg(err));
        return;
    }

    const QString modelVersion = QFileInfo(AppConfig::instance().arcfaceBin).baseName();
    if (!m_db->registerFace(employeeNo, feature, modelVersion, err)) {
        emit registerFinished(false, err);
        return;
    }

    // 保存注册用的人脸图（与识别抓拍分目录存放，便于人工核对）。
    // 图片只是辅助留档，特征已经入库，所以保存失败不影响注册结果，
    // 但必须让用户知道，不能伪装成完全成功。
    const QString capturePath = saveCapture(aligned, employeeNo, QStringLiteral("register"));

    // 提示该工号已有几张模板，方便知道还差几个方向没采
    QString cntErr;
    const int templates = m_db->templateCount(employeeNo, cntErr);
    QString message = QStringLiteral("注册成功: %1 %2，该工号模板 %3/%4 张，库内共 %5 人")
                          .arg(employeeNo, employeeName)
                          .arg(templates > 0 ? templates : 1)
                          .arg(FaceDatabase::kMaxTemplatesPerEmployee)
                          .arg(m_db->cachedEmployeeCount());
    if (capturePath.isEmpty()) {
        message += QStringLiteral("；注意：注册图未保存到 %1，请检查目录权限与磁盘空间")
                       .arg(QDir::toNativeSeparators(AppConfig::instance().imageSaveDir));
    }
    emit registerFinished(true, message);
}

void FacePipeline::lookupEmployee(const QString &employeeNo)
{
    if (!m_ready) {
        emit employeeLookupFinished(false, employeeNo, QString(), QString(),
                                    QStringLiteral("人脸流水线尚未就绪"));
        return;
    }

    QString name;
    QString department;
    QString error;
    if (!m_db->activeEmployee(employeeNo.trimmed(), name, department, error)) {
        emit employeeLookupFinished(false, employeeNo, QString(), QString(), error);
        return;
    }

    QString message = QStringLiteral("人员有效: %1").arg(name.isEmpty() ? employeeNo : name);
    if (!department.isEmpty()) message += QStringLiteral(" · ") + department;
    emit employeeLookupFinished(true, employeeNo, name, department, message);
}

void FacePipeline::syncExternalChanges()
{
    if (!m_ready || m_db == nullptr) return;

    bool changed = false;
    QString error;
    if (!m_db->syncCacheIfChanged(changed, error)) {
        if (!m_syncErrorActive) {
            emit logMessage(QStringLiteral("同步 MySQL 人员数据失败，将继续使用旧缓存: ") + error);
            m_syncErrorActive = true;
        }
        return;
    }

    if (m_syncErrorActive) {
        emit logMessage(QStringLiteral("MySQL 人员数据同步已恢复"));
        m_syncErrorActive = false;
    }
    if (changed) {
        emit logMessage(QStringLiteral(
                            "MySQL 人员数据已更新，当前启用 %1 人 / %2 张模板")
                            .arg(m_db->cachedEmployeeCount())
                            .arg(m_db->cachedFeatureCount()));
    }
}

void FacePipeline::reloadLogs(int limit)
{
    if (!m_ready) { emit logsLoaded({}); return; }
    QString err;
    const QVector<FaceData::LogRow> rows = m_db->recentLogs(limit, err);
    if (!err.isEmpty()) emit logMessage(err);
    emit logsLoaded(rows);
}

void FacePipeline::setRecognitionEnabled(bool on)
{
    m_recognitionEnabled = on;
}

void FacePipeline::setThreshold(float threshold)
{
    m_threshold = threshold;
}
