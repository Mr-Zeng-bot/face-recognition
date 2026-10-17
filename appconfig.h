#ifndef APPCONFIG_H
#define APPCONFIG_H

// ============================================================================
// appconfig.h —— 全局配置读取（config.ini）
// 说明：
//   * 把数据库、模型路径、识别阈值、相机等可调参数集中到 config.ini，
//     避免在代码里硬编码路径与账号，方便现场部署时修改。
//   * 通过 AppConfig::instance().load(path) 载入，程序启动时调用一次即可。
// ============================================================================

#include <QString>

class AppConfig
{
public:
    // 单例入口
    static AppConfig &instance();

    // 从 ini 文件载入配置；文件不存在时使用内置默认值并返回 false
    bool load(const QString &iniPath);

    // 配置文件路径（用于界面展示 / 排错）
    QString configPath() const { return m_configPath; }

    // ---- 数据库（MySQL） ----
    QString dbHost = QStringLiteral("127.0.0.1");
    int     dbPort = 3306;
    QString dbName = QStringLiteral("face_db");
    QString dbUser = QStringLiteral("root");
    QString dbPassword = QStringLiteral("root");
    // MySQL 驱动名：Qt6 官方安装包默认不含 QMYSQL，需自行编译驱动，
    // 详见 docs 中的“MySQL 驱动”章节。
    QString dbDriver = QStringLiteral("QMYSQL");
    // 配置读取/DPAPI 解密失败时由 FaceDatabase::open 转成明确的初始化错误。
    QString configError;
    // 人员/特征数据由 MySQL 管理，程序按此周期检查缓存版本并自动刷新。
    int dbCacheSyncIntervalMs = 1000;

    // ---- 人脸模型（ncnn） ----
    QString scrfdParam = QStringLiteral("models/scrfd_500m.param");
    QString scrfdBin   = QStringLiteral("models/scrfd_500m.bin");
    int     scrfdInputSize = 640;      // SCRFD 输入边长（500M 常用 640）
    float   detScoreThreshold = 0.5f;  // 检测分数阈值
    float   detNmsThreshold = 0.4f;    // NMS IoU 阈值

    QString arcfaceParam = QStringLiteral("models/arcface_mbf.param");
    QString arcfaceBin   = QStringLiteral("models/arcface_mbf.bin");
    int     arcfaceFeatureDim = 512;   // ArcFace 输出特征维度

    // ---- 识别阈值 ----
    // 余弦相似度阈值：越高越严格。ArcFace 常用 0.5 ~ 0.6。
    float matchThreshold = 0.5f;

    // ---- 人脸质量阈值 ----
    float minFaceWidth = 60.0f;   // 人脸框最小宽度（像素）
    float minBlurScore = 100.0f;  // Laplacian 方差下限（低于判定为模糊）
    float minBrightness = 40.0f;  // 平均亮度下限（低于判定为过暗）
    float maxBrightness = 220.0f; // 平均亮度上限（高于判定为过曝）

    // ---- 相机默认参数 ----
    QString imageSaveDir = QStringLiteral("captures");  // 抓拍图片保存目录

    // ---- 其它 ----
    bool useVulkan = false;        // 是否用 ncnn Vulkan 加速（默认 CPU，最稳）
    int  numThreads = 4;           // ncnn 推理线程数

private:
    AppConfig() = default;
    QString m_configPath;
};

#endif // APPCONFIG_H
