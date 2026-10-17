# ============================================================================
# set-db-password-dpapi.ps1
# 使用 Windows DPAPI（CurrentUser 范围）加密 MySQL 运行账号口令，并写入
# config.ini 的 passwordDpapi 字段，同时清空明文 password。
#
# 必须由实际启动 QT_Face.exe 的 Windows 用户执行。
# ============================================================================

param(
    [string]$ConfigPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'config.ini'),
    [System.Security.SecureString]$Password
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ConfigPath)) {
    throw "Config file not found: $ConfigPath"
}

Add-Type -AssemblyName System.Security

if ($null -eq $Password) {
    $Password = Read-Host 'Input MySQL password for face_app' -AsSecureString
}

$bstr = [System.IntPtr]::Zero
$plainBytes = $null
try {
    $bstr = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
    $plain = [System.Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
    if ([string]::IsNullOrEmpty($plain)) {
        throw 'Password cannot be empty'
    }

    $plainBytes = [System.Text.Encoding]::UTF8.GetBytes($plain)
    $cipherBytes = [System.Security.Cryptography.ProtectedData]::Protect(
        $plainBytes,
        $null,
        [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
    $encoded = [Convert]::ToBase64String($cipherBytes)
}
finally {
    if ($bstr -ne [System.IntPtr]::Zero) {
        [System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
    if ($null -ne $plainBytes) {
        [Array]::Clear($plainBytes, 0, $plainBytes.Length)
    }
    $plain = $null
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$sourceLines = [System.IO.File]::ReadAllLines($ConfigPath, $utf8NoBom)
$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.AddRange([string[]]$sourceLines)

$inDatabase = $false
$databaseFound = $false
$passwordIndex = -1
$dpapiIndex = -1

for ($i = 0; $i -lt $lines.Count; ++$i) {
    if ($lines[$i] -match '^\s*\[([^\]]+)\]\s*$') {
        $inDatabase = ($Matches[1] -eq 'Database')
        if ($inDatabase) { $databaseFound = $true }
        continue
    }
    if (-not $inDatabase) { continue }
    if ($lines[$i] -match '^\s*password\s*=') { $passwordIndex = $i }
    if ($lines[$i] -match '^\s*passwordDpapi\s*=') { $dpapiIndex = $i }
}

if (-not $databaseFound) {
    throw 'Section [Database] not found in config.ini'
}
if ($passwordIndex -lt 0) {
    throw 'password= line not found in section [Database]'
}

$lines[$passwordIndex] = 'password='
$dpapiLine = 'passwordDpapi=' + $encoded
if ($dpapiIndex -ge 0) {
    $lines[$dpapiIndex] = $dpapiLine
} else {
    $lines.Insert($passwordIndex + 1, $dpapiLine)
}

[System.IO.File]::WriteAllLines($ConfigPath, $lines, $utf8NoBom)

Write-Host "DPAPI password saved for the current Windows user."
Write-Host "Config: $ConfigPath"
Write-Host "The encrypted value can normally be decrypted only by the same Windows user on this machine."
