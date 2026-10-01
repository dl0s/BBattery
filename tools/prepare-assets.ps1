param([string]$ReferenceRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $ReferenceRoot) { $ReferenceRoot = Join-Path (Split-Path -Parent $root) 'BBFile' }
$icons = Join-Path $root 'assets/icons'
New-Item -ItemType Directory -Force -Path $icons | Out-Null
$mapping = @{ 'refresh'='refresh'; 'settings'='settings'; 'save'='save'; 'close'='close'; 'history'='history'; 'info'='info'; 'charge'='next'; 'discharge'='previous'; 'previous'='previous'; 'next'='next' }
foreach ($name in $mapping.Keys) {
    Copy-Item -LiteralPath (Join-Path $ReferenceRoot ('assets/icons/' + $mapping[$name] + '.png')) -Destination (Join-Path $icons ($name + '.png'))
}
Add-Type -AssemblyName System.Drawing
function Draw-Battery([int]$size, [bool]$background) {
    $image = [Drawing.Bitmap]::new($size,$size)
    $g = [Drawing.Graphics]::FromImage($image)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear($(if ($background) { [Drawing.ColorTranslator]::FromHtml('#151719') } else { [Drawing.Color]::Transparent }))
    $g.ScaleTransform($size/512.0,$size/512.0)
    $pen = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#63d6a0'),20)
    $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $g.DrawRectangle($pen,84,152,324,206)
    $brush = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#63d6a0'))
    $g.FillRectangle($brush,416,212,20,84)
    $trace = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#78c9ee'),15)
    $trace.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $points = [Drawing.PointF[]]@([Drawing.PointF]::new(124,296),[Drawing.PointF]::new(181,274),[Drawing.PointF]::new(219,282),[Drawing.PointF]::new(268,235),[Drawing.PointF]::new(312,246),[Drawing.PointF]::new(366,198))
    $g.DrawLines($trace,$points)
    $pen.Dispose(); $trace.Dispose(); $brush.Dispose(); $g.Dispose()
    return $image
}
$icon = Draw-Battery 114 $true
try { $icon.Save((Join-Path $root 'assets/icon.png'),[Drawing.Imaging.ImageFormat]::Png) } finally { $icon.Dispose() }
$battery = Draw-Battery 81 $false
try { $battery.Save((Join-Path $icons 'battery.png'),[Drawing.Imaging.ImageFormat]::Png) } finally { $battery.Dispose() }
$trend = [Drawing.Bitmap]::new(81,81)
$graphics = [Drawing.Graphics]::FromImage($trend)
try {
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([Drawing.Color]::Transparent)
    $pen = [Drawing.Pen]::new([Drawing.Color]::White,4)
    $graphics.DrawLine($pen,15,15,15,66)
    $graphics.DrawLine($pen,15,66,68,66)
    $graphics.DrawLines($pen,[Drawing.PointF[]]@([Drawing.PointF]::new(24,54),[Drawing.PointF]::new(37,37),[Drawing.PointF]::new(48,46),[Drawing.PointF]::new(65,22)))
    $trend.Save((Join-Path $icons 'trend.png'),[Drawing.Imaging.ImageFormat]::Png)
    $pen.Dispose()
} finally { $graphics.Dispose(); $trend.Dispose() }
