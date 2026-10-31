<#
.SYNOPSIS
    由 res/msixpkg.png（1024×1024 母版）生成 Windows 多尺寸图标 res/msixpkg.ico。

.DESCRIPTION
    为什么要有这个脚本：msixpkg.ico 同时被三处使用 —— 原生向导/CLI 的 PE 资源
    （res/msixpkg.rc）、setup / Uninstall.exe 的安装器图标、以及 .msixpkg 文件关联的
    DefaultIcon（从 msixpkg-installer.exe 取）。
    母版 PNG 只有一个尺寸，直接让 rc.exe 去缩放会在 16×16 下糊成一团，
    所以这里一次性烘出 Windows 会实际用到的全部尺寸。

    尺寸选择：
      16 / 20 / 24 / 32 / 40 / 48 —— Explorer 列表、任务栏、通知区域在各 DPI 下的档位；
      64 / 96 / 128              —— 大图标视图与「属性 → 详细信息」；
      256                        —— 超大图标与安装包展示。

    编码选择：
      * 16–128 写 32bpp DIB（BITMAPINFOHEADER + BGRA + AND mask）。这是资源管理器
        能稳定读取的老格式；
      * 256 写 PNG（Vista+ 支持）。256 的 DIB 会白白多出 256 KB，而 PNG 更小。

    透明处理：
      母版四角是透明的。GDI+ 直接缩小直通 alpha 图会在圆角处留下一圈暗边，
      因此先把母版画到 Format32bppPArgb（预乘 alpha）位图上，让 GDI+ 在预乘空间
      里做双三次采样（HighQualityBicubic），再转回直通 alpha 写进 ICO。

.PARAMETER SourcePng
    母版 PNG，默认 src/MsixpkgInstaller.Native/res/msixpkg.png。

.PARAMETER OutputIco
    输出 ICO，默认 src/MsixpkgInstaller.Native/res/msixpkg.ico（会覆盖）。

.PARAMETER Sizes
    要包含的尺寸；默认 16,20,24,32,40,48,64,96,128,256。超过 256 的尺寸会被拒绝。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build-app-icon.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build-app-icon.ps1 -Sizes 16,32,48,256
#>
[CmdletBinding()]
param(
    [string]$SourcePng,
    [string]$OutputIco,
    [int[]]$Sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $SourcePng) {
    $SourcePng = Join-Path $repoRoot 'src\MsixpkgInstaller.Native\res\msixpkg.png'
}
if (-not $OutputIco) {
    $OutputIco = Join-Path $repoRoot 'src\MsixpkgInstaller.Native\res\msixpkg.ico'
}

if (-not (Test-Path -LiteralPath $SourcePng)) {
    throw "找不到母版 PNG：$SourcePng"
}
if ($Sizes.Count -eq 0) { throw '-Sizes 不能为空。' }
foreach ($size in $Sizes) {
    if ($size -lt 16 -or $size -gt 256) {
        throw "尺寸 $size 不合法：只支持 16–256（Windows 图标目录项的宽高是 1 字节，256 用 0 表示）。"
    }
}
$Sizes = @($Sizes | Sort-Object -Unique)

Add-Type -AssemblyName System.Drawing

function Resize-IconBitmap {
    param(
        [System.Drawing.Image]$Source,
        [int]$Size
    )

    # 先画进预乘 alpha 位图：缩小透明图时，未预乘的 RGB 会把透明像素的黑色
    # 混进圆角边缘（一圈暗边）。预乘后 GDI+ 的插值就不会污染颜色。
    $premultiplied = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppPArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($premultiplied)
    try {
        $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.DrawImage($Source, (New-Object System.Drawing.Rectangle(0, 0, $Size, $Size)))
    } finally {
        $graphics.Dispose()
    }

    # ICO 的 DIB 条目要直通 alpha；PNG 条目同样按直通 alpha 编码。
    $straight = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($straight)
    try {
        $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.DrawImage($premultiplied, 0, 0)
    } finally {
        $graphics.Dispose()
        $premultiplied.Dispose()
    }

    return $straight
}

function Get-PngBytes {
    param([System.Drawing.Bitmap]$Bitmap)

    $stream = New-Object System.IO.MemoryStream
    try {
        $Bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        return $stream.ToArray()
    } finally {
        $stream.Dispose()
    }
}

function Get-DibBytes {
    param([System.Drawing.Bitmap]$Bitmap)

    $size = $Bitmap.Width
    $rect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
    $locked = $Bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        # Format32bppArgb 的内存布局就是 ICO DIB 要的 BGRA（每行按 stride 对齐）。
        $xor = New-Object byte[] ($locked.Stride * $size)
        [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $xor, 0, $xor.Length)
        $stride = $locked.Stride
    } finally {
        $Bitmap.UnlockBits($locked)
    }

    # AND mask：每行按 32 位对齐；alpha == 0 的像素置 1（“透明”）。
    # 32bpp 图标其实靠 alpha 通道，但老组件（部分 Shell 路径）仍会读 mask。
    $maskStride = [int]([Math]::Ceiling($size / 32.0) * 4)
    $mask = New-Object byte[] ($maskStride * $size)
    for ($y = 0; $y -lt $size; $y++) {
        $rowBase = $y * $stride
        $maskBase = $y * $maskStride
        for ($x = 0; $x -lt $size; $x++) {
            if ($xor[$rowBase + ($x * 4) + 3] -eq 0) {
                $mask[$maskBase + ($x -shr 3)] = [byte]($mask[$maskBase + ($x -shr 3)] -bor (0x80 -shr ($x -band 7)))
            }
        }
    }

    $stream = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter($stream)
    try {
        # BITMAPINFOHEADER。biHeight 是 XOR + AND 两张图的高度之和。
        $writer.Write([uint32]40)                 # biSize
        $writer.Write([int32]$size)               # biWidth
        $writer.Write([int32]($size * 2))         # biHeight
        $writer.Write([uint16]1)                  # biPlanes
        $writer.Write([uint16]32)                 # biBitCount
        $writer.Write([uint32]0)                  # biCompression = BI_RGB
        $writer.Write([uint32]($size * $size * 4 + $mask.Length)) # biSizeImage
        $writer.Write([int32]0)                   # biXPelsPerMeter
        $writer.Write([int32]0)                   # biYPelsPerMeter
        $writer.Write([uint32]0)                  # biClrUsed
        $writer.Write([uint32]0)                  # biClrImportant

        # DIB 行序自下而上。
        for ($y = $size - 1; $y -ge 0; $y--) {
            $writer.Write($xor, $y * $stride, $size * 4)
        }
        for ($y = $size - 1; $y -ge 0; $y--) {
            $writer.Write($mask, $y * $maskStride, $maskStride)
        }

        $writer.Flush()
        return $stream.ToArray()
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

$source = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $SourcePng).Path)
if ($source.Width -ne $source.Height) {
    $sourceWidth = $source.Width
    $sourceHeight = $source.Height
    $source.Dispose()
    throw "母版必须是正方形：$SourcePng 是 $sourceWidth×$sourceHeight。"
}

$entries = New-Object System.Collections.Generic.List[object]
try {
    foreach ($size in $Sizes) {
        $bitmap = Resize-IconBitmap -Source $source -Size $size
        try {
            if ($size -ge 256) {
                $bytes = Get-PngBytes -Bitmap $bitmap
                $kind = 'PNG'
            } else {
                $bytes = Get-DibBytes -Bitmap $bitmap
                $kind = 'DIB'
            }
        } finally {
            $bitmap.Dispose()
        }

        # ICO 目录项：宽高各 1 字节，256 用 0 表示。
        $directoryWidth = [byte]0
        $directoryHeight = [byte]0
        if ($size -lt 256) {
            $directoryWidth = [byte]$size
            $directoryHeight = [byte]$size
        }

        $entries.Add([pscustomobject]@{
                Size   = $size
                Kind   = $kind
                Width  = $directoryWidth
                Height = $directoryHeight
                Bytes  = $bytes
            })
    }
} finally {
    $source.Dispose()
}

# ICO 容器：6 字节头 + 每项 16 字节目录项 + 各尺寸图像数据。
$outputDir = Split-Path -Parent $OutputIco
if ($outputDir -and -not (Test-Path -LiteralPath $outputDir)) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
try {
    $writer.Write([uint16]0)                    # reserved
    $writer.Write([uint16]1)                    # type = icon
    $writer.Write([uint16]$entries.Count)

    $offset = 6 + (16 * $entries.Count)
    foreach ($entry in $entries) {
        $writer.Write([byte]$entry.Width)
        $writer.Write([byte]$entry.Height)
        $writer.Write([byte]0)                  # color count（真彩色写 0）
        $writer.Write([byte]0)                  # reserved
        $writer.Write([uint16]1)                # planes
        $writer.Write([uint16]32)               # bit count
        $writer.Write([uint32]$entry.Bytes.Length)
        $writer.Write([uint32]$offset)
        $offset += $entry.Bytes.Length
    }
    foreach ($entry in $entries) {
        # 显式转成 byte[]：否则 PowerShell 的重载解析会挑 Write(byte)，只写第一个字节。
        $writer.Write([byte[]]$entry.Bytes)
    }

    $writer.Flush()
    [System.IO.File]::WriteAllBytes($OutputIco, $stream.ToArray())
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

$icoLength = (Get-Item -LiteralPath $OutputIco).Length
$sizeSummary = ($entries | ForEach-Object { "$($_.Size)($($_.Kind))" }) -join ', '
Write-Host "生成：$OutputIco"
Write-Host "  尺寸：$sizeSummary"
Write-Host "  大小：$icoLength 字节"
