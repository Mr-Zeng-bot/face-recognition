#include "appconfig.h"

#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QByteArray>
#include <climits>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif
#endif

namespace {

bool decryptDpapiPassword(const QString &encoded, QString &password, QString &error)
{
#ifdef Q_OS_WIN
    const QByteArray cipher = QByteArray::fromBase64(
        encoded.trimmed().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (cipher.isEmpty()) {
        error = QStringLiteral("passwordDpapi 不是有效的 Base64 密文");
        return false;
    }
    if (cipher.size() > static_cast<int>(INT_MAX)) {
        error = QStringLiteral("passwordDpapi 数据过大");
        return false;
    }

    DATA_BLOB input = {};
    input.cbData = static_cast<DWORD>(cipher.size());
    input.pbData = reinterpret_cast<BYTE *>(
        const_cast<char *>(cipher.constData()));
    DATA_BLOB output = {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        const DWORD code = GetLastError();
        error = QStringLiteral(
                    "数据库口令 DPAPI 解密失败（Windows 错误码 0x%1）。"
                    "该密文通常只能由加密它的 Windows 用户在同一台机器上解密；"
                    "请用程序运行用户重新执行 tools\\set-db-password-dpapi.ps1。")
                    .arg(code, 8, 16, QLatin1Char('0'));
        return false;
    }

    QByteArray plain(reinterpret_cast<const char *>(output.pbData),
                     static_cast<int>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);

    password = QString::fromUtf8(plain);
    plain.fill('\0');
    if (password.isEmpty()) {
        error = QStringLiteral("DPAPI 解密后的数据库口令为空");
        return false;
    }
    return true;
#else
    Q_UNUSED(encoded);
    Q_UNUSED(password);
    error = QStringLiteral("当前平台不支持 Windows DPAPI");
    return false;
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// AppConfig 实现
// ---------------------------------------------------------------------------

AppConfig &AppConfig::instance()
{
    // C++11 起函数局部静态变量初始化是线程安全的，作为单例最简单可靠。
    static AppConfig s_instance;
    return s_instance;
}

bool AppConfig::load(const QString &iniPath)
{
    m_configPath = iniPath;
    configError.clear();

    const bool exists = QFileInfo::exists(iniPath);
    if (!exists) {
        // 缺少配置只能由运行账号/端口等内置默认值兜底，且可能静默连到错误账号，
        // 因此直接记为配置错误，由 FaceDatabase::open 在连库前明确报出。
        configError = QStringLiteral(
                          "找不到配置文件 %1，请确认 config.ini 与程序位于同一目录")
                          .arg(iniPath);
    }
    // QSettings 使用 IniFormat，即使文件不存在也会以默认值返回，不会抛异常。
    // 注：Qt6 的 INI 文件固定使用 UTF-8，已移除 Qt5 时代的 setIniCodec()。
    QSettings settings(iniPath, QSettings::IniFormat);

    // ---- 数据库 ----
    settings.beginGroup("Database");
    dbHost     = settings.value("host", dbHost).toString();
    dbPort     = settings.value("port", dbPort).toInt();
    dbName     = settings.value("name", dbName).toString();
    dbUser     = settings.value("user", dbUser).toString();
    const QString plainPassword =
        settings.value("password", QString()).toString();
    const QString dpapiPassword =
        settings.value("passwordDpapi", QString()).toString();
    if (!dpapiPassword.isEmpty()) {
        QString decrypted;
        if (!decryptDpapiPassword(dpapiPassword, decrypted, configError)) {
            dbPassword.clear();
        } else {
            dbPassword = decrypted;
        }
    } else {
        // 兼容旧配置；迁移到 DPAPI 后应清空明文 password。
        dbPassword = plainPassword;
    }
    dbDriver   = settings.value("driver", dbDriver).toString();
    dbCacheSyncIntervalMs =
        settings.value("cacheSyncIntervalMs", dbCacheSyncIntervalMs).toInt();
    settings.endGroup();

    // ---- 检测模型 ----
    settings.beginGroup("Detector");
    scrfdParam        = settings.value("param", scrfdParam).toString();
    scrfdBin          = settings.value("bin", scrfdBin).toString();
    scrfdInputSize    = settings.value("inputSize", scrfdInputSize).toInt();
    detScoreThreshold = settings.value("scoreThreshold", detScoreThreshold).toFloat();
    detNmsThreshold   = settings.value("nmsThreshold", detNmsThreshold).toFloat();
    settings.endGroup();

    // ---- 特征提取模型 ----
    settings.beginGroup("Extractor");
    arcfaceParam      = settings.value("param", arcfaceParam).toString();
    arcfaceBin        = settings.value("bin", arcfaceBin).toString();
    arcfaceFeatureDim = settings.value("featureDim", arcfaceFeatureDim).toInt();
    settings.endGroup();

    // ---- 识别阈值 ----
    settings.beginGroup("Recognition");
    matchThreshold = settings.value("matchThreshold", matchThreshold).toFloat();
    settings.endGroup();

    // ---- 人脸质量 ----
    settings.beginGroup("Quality");
    minFaceWidth   = settings.value("minFaceWidth", minFaceWidth).toFloat();
    minBlurScore   = settings.value("minBlurScore", minBlurScore).toFloat();
    minBrightness  = settings.value("minBrightness", minBrightness).toFloat();
    maxBrightness  = settings.value("maxBrightness", maxBrightness).toFloat();
    settings.endGroup();

    // ---- 图片保存目录 ----
    settings.beginGroup("Storage");
    imageSaveDir = settings.value("imageDir", imageSaveDir).toString();
    settings.endGroup();

    // ---- 推理 ----
    settings.beginGroup("Inference");
    useVulkan  = settings.value("useVulkan", useVulkan).toBool();
    numThreads = settings.value("numThreads", numThreads).toInt();
    settings.endGroup();

    // 配置里的相对路径统一按「本 ini 文件所在目录」解析（部署时该文件就在 exe 目录）。
    // 若按当前工作目录解析，换一种启动方式（例如"起始位置"非 exe 目录的快捷方式）
    // 就会找不到模型、抓拍图也会落到别处，人脸模块会直接初始化失败。
    const QString baseDir = QFileInfo(iniPath).absolutePath();
    const auto resolvePath = [&baseDir](QString &path) {
        if (!path.isEmpty() && QDir::isRelativePath(path))
            path = QDir(baseDir).filePath(path);
    };
    resolvePath(scrfdParam);
    resolvePath(scrfdBin);
    resolvePath(arcfaceParam);
    resolvePath(arcfaceBin);
    resolvePath(imageSaveDir);

    // 保证抓拍目录存在
    if (!imageSaveDir.isEmpty())
        QDir().mkpath(imageSaveDir);

    return exists;
}
