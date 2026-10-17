# ============================================================================
# deploy-runtime.ps1 —— 构建后部署脚本
#   把可执行文件运行所需的运行时与资源拷贝到构建输出目录，使其可脱离
#   Qt Creator 直接运行：
#     1) Qt 运行时（windeployqt，含 platforms 插件、MSVC 运行库）
#     2) ncnn.dll（shared 版 ncnn 必需）
#     3) 海康 MVS 运行时目录
#     4) OpenCV 运行时（opencv_world + opencv_videoio_ffmpeg）
#     5) 人脸模型（models 目录）
#     6) MSVC Release 运行库（仅 release 配置）
#     7) MySQL 客户端库 libmysql.dll（QMYSQL 驱动依赖）
# ============================================================================
param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$MvsRuntime,
    [Parameter(Mandatory=$true)][string]$OpenCvBin,
    [Parameter(Mandatory=$true)][string]$OpenCvLibrary,
    [Parameter(Mandatory=$true)][string]$NcnnBin,
    [Parameter(Mandatory=$true)][string]$ModelsDir,
    [Parameter(Mandatory=$true)][string]$QtDeployTool,
    [Parameter(Mandatory=$true)][ValidateSet('debug', 'release')][string]$Configuration,
    # 可选：MySQL 客户端库所在目录；留空则自动在常见位置查找
    [string]$MySqlLibDir = ''
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$application = Join-Path $Destination 'QT_Face.exe'
if (-not (Test-Path -LiteralPath $application)) {
    throw "Application not found: $application"
}
if (-not (Test-Path -LiteralPath $QtDeployTool)) {
    throw "windeployqt not found: $QtDeployTool"
}

# ---- 1) Qt 运行时 + MSVC 运行库 ----
# 注意：windeployqt 会把“警告”写到 stderr，而 $ErrorActionPreference='Stop' 会把
# 原生命令的 stderr 文本当作致命错误处理。这里临时放宽，仅以退出码判断成败。
$qtMode = '--' + $Configuration
$previousEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $QtDeployTool $qtMode --compiler-runtime --no-translations --force --dir $Destination $application
$deployExit = $LASTEXITCODE
$ErrorActionPreference = $previousEap
if ($deployExit -ne 0) {
    throw "windeployqt failed with exit code $deployExit"
}

# ---- 2) ncnn 运行时 ----
if (Test-Path -LiteralPath (Join-Path $NcnnBin 'ncnn.dll')) {
    Copy-Item -LiteralPath (Join-Path $NcnnBin 'ncnn.dll') -Destination $Destination -Force
} else {
    Write-Warning "ncnn.dll 未找到: $NcnnBin"
}

# ---- 3) MVS 运行时（含 ThirdParty 目录） ----
Get-ChildItem -LiteralPath $MvsRuntime | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
}

# ---- 4) OpenCV 运行时 ----
Copy-Item -LiteralPath (Join-Path $OpenCvBin ($OpenCvLibrary + '.dll')) -Destination $Destination -Force
Get-ChildItem -LiteralPath $OpenCvBin -Filter 'opencv_videoio_*.dll' | ForEach-Object {
    $isDebugDll = $_.BaseName.EndsWith('d')
    if ($_.Name -like 'opencv_videoio_ffmpeg*' -or
        ($Configuration -eq 'debug' -and $isDebugDll) -or
        ($Configuration -eq 'release' -and -not $isDebugDll)) {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Force
    }
}

# ---- 5) 人脸模型 ----
$modelTarget = Join-Path $Destination 'models'
New-Item -ItemType Directory -Force -Path $modelTarget | Out-Null
if (Test-Path -LiteralPath $ModelsDir) {
    Get-ChildItem -LiteralPath $ModelsDir -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $modelTarget -Force
    }
} else {
    Write-Warning "模型目录不存在: $ModelsDir"
}

# ---- 6) 配置文件 ----
# 优先使用已经配置好的 config.ini；若源码里还没有（例如刚克隆仓库），退回
# config.ini.example 模板，保证部署目录里始终有一份可编辑的 config.ini，
# 这样程序报的是“连不上 MySQL”，而不是“找不到配置文件”直接拒绝启动。
$configSource = Join-Path (Split-Path -Parent $PSScriptRoot) 'config.ini'
if (-not (Test-Path -LiteralPath $configSource)) {
    $configSource = Join-Path $PSScriptRoot 'config.ini'
}
if (-not (Test-Path -LiteralPath $configSource)) {
    $configSource = Join-Path (Split-Path -Parent $PSScriptRoot) 'config.ini.example'
    if (-not (Test-Path -LiteralPath $configSource)) {
        $configSource = Join-Path $PSScriptRoot 'config.ini.example'
    }
}
if (Test-Path -LiteralPath $configSource) {
    $configTarget = Join-Path $Destination 'config.ini'
    if (Test-Path -LiteralPath $configTarget) {
        # 已存在的配置可能带有现场写入的 DPAPI 密文，覆盖会让 DPAPI 失效
        Write-Host 'config.ini 已存在，跳过覆盖'
    } else {
        # 来源无论是 config.ini 还是模板，部署目录里统一命名为 config.ini
        Copy-Item -LiteralPath $configSource -Destination $configTarget -Force
        if ($configSource -like '*.example') {
            Write-Host '已从 config.ini.example 生成 config.ini，请在部署目录中补齐数据库账号与口令'
        }
    }
}

# ---- 6b) 数据库初始化/迁移脚本 ----
$sqlSource = Join-Path $PSScriptRoot 'sql'
if (Test-Path -LiteralPath $sqlSource) {
    $sqlTarget = Join-Path $Destination 'sql'
    New-Item -ItemType Directory -Force -Path $sqlTarget | Out-Null
    Get-ChildItem -LiteralPath $sqlSource -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $sqlTarget -Force
    }
} else {
    Write-Warning "SQL directory not found: $sqlSource"
}

# ---- 6c) 部署与迁移文档 ----
$docsSource = Join-Path $PSScriptRoot 'docs'
if (Test-Path -LiteralPath $docsSource) {
    $docsTarget = Join-Path $Destination 'docs'
    New-Item -ItemType Directory -Force -Path $docsTarget | Out-Null
    Get-ChildItem -LiteralPath $docsSource -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $docsTarget -Force
    }
} else {
    Write-Warning "Docs directory not found: $docsSource"
}

# ---- 6d) DPAPI 配置工具 ----
$toolsSource = Join-Path $PSScriptRoot 'tools'
if (Test-Path -LiteralPath $toolsSource) {
    $toolsTarget = Join-Path $Destination 'tools'
    New-Item -ItemType Directory -Force -Path $toolsTarget | Out-Null
    Get-ChildItem -LiteralPath $toolsSource -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $toolsTarget -Force
    }
} else {
    Write-Warning "Tools directory not found: $toolsSource"
}

# ---- 7) MSVC Release 运行库 ----
if ($Configuration -eq 'release') {
    $redistRoots = @()
    if ($env:VCToolsRedistDir) { $redistRoots += $env:VCToolsRedistDir }
    foreach ($vsRoot in @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
        'C:\Program Files\Microsoft Visual Studio\18\Community',
        'C:\Program Files\Microsoft Visual Studio\18\Professional',
        'C:\Program Files\Microsoft Visual Studio\18\Enterprise',
        'E:\VS2022'
    )) {
        $redistRoots += (Join-Path $vsRoot 'VC\Redist\MSVC')
    }
    $msvcRuntimeDir = $null
    foreach ($redistRoot in $redistRoots | Select-Object -Unique) {
        if (-not (Test-Path -LiteralPath $redistRoot)) { continue }
        $toolsetDirs = @($redistRoot)
        $toolsetDirs += Get-ChildItem -LiteralPath $redistRoot -Directory -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName
        foreach ($toolsetDir in $toolsetDirs) {
            $archDir = Join-Path $toolsetDir 'x64'
            if (-not (Test-Path -LiteralPath $archDir)) { continue }
            $msvcRuntimeDir = Get-ChildItem -LiteralPath $archDir -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
            if ($msvcRuntimeDir) { break }
        }
        if ($msvcRuntimeDir) { break }
    }
    if ($msvcRuntimeDir) {
        Get-ChildItem -LiteralPath $msvcRuntimeDir -Filter '*.dll' -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $Destination -Force
        }
    } else {
        Write-Warning '未找到 MSVC release 运行库，请在目标机器安装 VC++ 运行库。'
    }
}

# ---- 8) MySQL 客户端库（QMYSQL 驱动运行期依赖 libmysql.dll） ----
# qsqlmysql.dll 由 windeployqt 从 Qt 插件目录（plugins/sqldrivers）一起拷过来，
# 但它还需要 libmysql.dll 才能加载，这里按常见安装位置自动查找。
if (Test-Path -LiteralPath (Join-Path $Destination 'libmysql.dll')) {
    Write-Host 'libmysql.dll 已存在，跳过'
} else {
    $mysqlCandidates = @()
    if ($MySqlLibDir) { $mysqlCandidates += $MySqlLibDir }
    foreach ($root in @("$env:ProgramFiles\MySQL", "${env:ProgramFiles(x86)}\MySQL")) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $mysqlCandidates += Get-ChildItem -LiteralPath $root -Directory -Filter 'MySQL Server *' -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'lib' }
    }
    $libMySql = $null
    foreach ($dir in $mysqlCandidates) {
        $candidate = Join-Path $dir 'libmysql.dll'
        if (Test-Path -LiteralPath $candidate) { $libMySql = $candidate; break }
    }
    if ($libMySql) {
        Copy-Item -LiteralPath $libMySql -Destination $Destination -Force
        Write-Host "libmysql.dll: $libMySql"
    } else {
        Write-Warning '未找到 libmysql.dll（QMYSQL 驱动必需），请手动拷贝到输出目录。'
    }
}

Write-Host ("Deployed Qt, ncnn, MSVC, MVS, OpenCV runtime and models ({0}) to {1}" -f $Configuration, $Destination)
