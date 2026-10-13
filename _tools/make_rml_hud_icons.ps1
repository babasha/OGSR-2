# Own artwork for the RmlUi HUD ("True Stalker" look, our drawing): vector shapes
# rendered with WPF to anti-aliased PNG, then (WSL ImageMagick) to DDS with mips.
#   powershell -ExecutionPolicy Bypass -File _tools\make_rml_hud_icons.ps1 [-Out <dir>]
# Output: <Out>\png\*.png. The DDS step is make_rml_hud_icons.sh.
# ASCII only (Windows PowerShell reads a BOM-less script in the ANSI codepage).
#
# Status icons are a dark disc with a thin ring and a WHITE glyph: the HUD tints
# the whole picture with image-color (beige / yellow / red by severity), which
# leaves the dark disc dark and colours the glyph and the ring. Glyph-only icons
# (noise, light) are white on transparent for the same reason.
param([string]$Out = "$PSScriptRoot\out_hud")

Add-Type -AssemblyName PresentationCore, PresentationFramework, WindowsBase
$ErrorActionPreference = 'Stop'
# ⚠path data is built with -f: a comma-decimal locale (this machine is pt-BR) wrote "57,00"
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture
New-Item -ItemType Directory -Force "$Out\png" | Out-Null

$white = [System.Windows.Media.Brushes]::White
function Geo([string]$s) { [System.Windows.Media.Geometry]::Parse($s) }
function Pen([double]$w) {
    $p = New-Object System.Windows.Media.Pen($white, $w)
    $p.StartLineCap = 'Round'; $p.EndLineCap = 'Round'; $p.LineJoin = 'Round'
    return $p
}
function Minus($a, $b) { New-Object System.Windows.Media.CombinedGeometry('Exclude', $a, $b) }

# One picture: $size px square, drawn in a 128-unit box; $layers = list of
# @{ g = geometry; fill = $true/$false; stroke = width }.
function Render([string]$name, [int]$size, [bool]$disc, $layers) {
    $dv = New-Object System.Windows.Media.DrawingVisual
    $dc = $dv.RenderOpen()
    $k = $size / 128.0
    $dc.PushTransform((New-Object System.Windows.Media.ScaleTransform($k, $k)))
    if ($disc) {
        $bg = New-Object System.Windows.Media.SolidColorBrush([System.Windows.Media.Color]::FromArgb(232, 20, 19, 16))
        $dc.DrawEllipse($bg, $null, (New-Object System.Windows.Point(64, 64)), 61, 61)
        $ring = New-Object System.Windows.Media.Pen((New-Object System.Windows.Media.SolidColorBrush([System.Windows.Media.Color]::FromArgb(150, 255, 255, 255))), 4)
        $dc.DrawEllipse($null, $ring, (New-Object System.Windows.Point(64, 64)), 58, 58)
    }
    foreach ($l in $layers) {
        $fill = if ($l.brush) { $l.brush } elseif ($l.fill) { $white } else { $null }
        $pen = if ($l.stroke) { Pen $l.stroke } else { $null }
        if ($pen -and $l.alpha) { $pen.Brush = New-Object System.Windows.Media.SolidColorBrush([System.Windows.Media.Color]::FromArgb($l.alpha, 255, 255, 255)) }
        $dc.DrawGeometry($fill, $pen, $l.g)
    }
    $dc.Pop()
    $dc.Close()
    $bmp = New-Object System.Windows.Media.Imaging.RenderTargetBitmap($size, $size, 96, 96, [System.Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($dv)
    # straight alpha for the PNG (the engine's UI blends straight alpha)
    $conv = New-Object System.Windows.Media.Imaging.FormatConvertedBitmap($bmp, [System.Windows.Media.PixelFormats]::Bgra32, $null, 0)
    $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
    $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($conv))
    $fs = [System.IO.File]::Create("$Out\png\$name.png")
    $enc.Save($fs); $fs.Close()
    Write-Output "$name ${size}px"
}
function F($g) { @{ g = $g; fill = $true } }
function S($g, $w) { @{ g = $g; stroke = $w } }

# ------------------------------------------------------------- the glyphs ---
# All in the 128 box, centred on 64,64, inside a ~76 px circle (the disc's ring is r58).

# radiation: three 60-degree blades between r14 and r38, a hub of r9
$blades = ''
foreach ($a0 in @(-120, 0, 120)) {
    $a1 = ($a0 - 30) * [Math]::PI / 180; $a2 = ($a0 + 30) * [Math]::PI / 180
    $p = { param($r, $a) "{0:F2},{1:F2}" -f (64 + $r * [Math]::Sin($a)), (64 - $r * [Math]::Cos($a)) }
    $blades += "M $(& $p 14 $a1) L $(& $p 38 $a1) A 38,38 0 0 1 $(& $p 38 $a2) L $(& $p 14 $a2) A 14,14 0 0 0 $(& $p 14 $a1) Z "
}
$rad = @((F (Geo $blades)), (F (Geo 'M 55,64 A 9,9 0 1 1 73,64 A 9,9 0 1 1 55,64 Z')))

# bleeding: a drop, and a smaller one beside it
$blood = @(
    (F (Geo 'M 58,26 C 58,26 36,54 36,72 A 22,22 0 0 0 80,72 C 80,54 58,26 58,26 Z')),
    (F (Geo 'M 86,58 C 86,58 76,70 76,78 A 10,10 0 0 0 96,78 C 96,70 86,58 86,58 Z'))
)

# hunger: a fork and a knife
$hunger = @(
    (F (Geo 'M 38,28 L 42,28 L 42,50 L 45,50 L 45,28 L 49,28 L 49,50 L 52,50 L 52,28 L 56,28 L 56,54 C 56,60 52,63 50,63 L 50,100 C 50,103 48,104 47,104 C 46,104 44,103 44,100 L 44,63 C 42,63 38,60 38,54 Z')),
    (F (Geo 'M 78,28 C 90,32 92,52 88,66 L 84,66 L 84,100 C 84,103 82,104 81,104 C 80,104 78,103 78,100 Z'))
)

# thirst: a bottle with a wave across it
$bottle = Geo 'M 58,22 L 70,22 L 70,36 C 70,40 82,44 82,54 L 82,100 C 82,104 79,106 76,106 L 52,106 C 49,106 46,104 46,100 L 46,54 C 46,44 58,40 58,36 Z'
$wave = Geo 'M 46,74 C 53,68 59,80 64,74 C 69,68 75,80 82,74 L 82,80 C 75,86 69,74 64,80 C 59,86 53,74 46,80 Z'
$thirst = @((F (Minus $bottle $wave)))

# psy: the letter psi, stroked
$psy = @(S (Geo 'M 64,28 L 64,102 M 38,34 C 38,62 48,72 64,72 C 80,72 90,62 90,34 M 50,102 L 78,102') 9)

# a jammed weapon: a pistol and an exclamation mark
$jam = @(
    (F (Geo 'M 22,40 L 84,40 L 84,56 L 52,56 L 48,62 L 46,90 C 46,92 44,94 42,94 L 32,94 C 30,94 28,92 29,90 L 34,56 L 22,56 Z')),
    (S (Geo 'M 52,56 C 52,68 62,68 64,58') 4),
    (F (Geo 'M 96,30 L 106,30 L 104,70 L 98,70 Z')),
    (F (Geo 'M 95,82 A 6,6 0 1 1 107,82 A 6,6 0 1 1 95,82 Z'))
)

# immortal: a five-point star
$star = ''
for ($i = 0; $i -lt 10; $i++) {
    $r = if ($i % 2) { 17 } else { 40 }
    $a = $i * [Math]::PI / 5
    $star += ("{0} {1:F2},{2:F2} " -f $(if ($i) { 'L' } else { 'M' }), (64 + $r * [Math]::Sin($a)), (66 - $r * [Math]::Cos($a)))
}
$god = @(F (Geo "$star Z"))

# sleep: a crescent and a small z
$moon = Minus (Geo 'M 30,66 A 32,32 0 1 1 94,66 A 32,32 0 1 1 30,66 Z') (Geo 'M 46,52 A 32,32 0 1 1 110,52 A 32,32 0 1 1 46,52 Z')
$zed = Geo 'M 80,26 L 100,26 L 100,31 L 87,43 L 100,43 L 100,48 L 80,48 L 80,43 L 93,31 L 80,31 Z'
$sleep = @((F $moon), (F $zed))
# cannot sleep: the crescent struck through
$nosleep = @((F $moon), (S (Geo 'M 30,98 L 98,30') 8))

# shelter: a roof over a wall
$shelter = @(
    (S (Geo 'M 26,62 L 64,30 L 102,62') 9),
    (F (Geo 'M 40,62 L 88,62 L 88,100 L 72,100 L 72,78 L 56,78 L 56,100 L 40,100 Z'))
)

# overweight: a scale weight -- a trapezoid with a ring on top (a kettlebell read
# as a padlock at this size, twice)
$body = Geo 'M 32,102 L 96,102 L 86,54 C 85,50 83,48 79,48 L 49,48 C 45,48 43,50 42,54 Z'
$weight = @((F $body), (S (Geo 'M 53,46 A 11,11 0 1 1 75,46') 7))

# ------------------------------------------------- glyph-only (no disc) ---
# noise: a speaker and two waves
$noise = @(
    (F (Geo 'M 18,50 L 38,50 L 62,28 L 62,100 L 38,78 L 18,78 Z')),
    (S (Geo 'M 78,46 C 86,54 86,74 78,82') 8),
    (S (Geo 'M 92,34 C 106,48 106,80 92,94') 8)
)
# light: an open eye
$eye = Minus (Geo 'M 14,64 C 34,30 94,30 114,64 C 94,98 34,98 14,64 Z') (Geo 'M 26,64 C 44,40 84,40 102,64 C 84,88 44,88 26,64 Z')
$light = @((F $eye), (F (Geo 'M 48,64 A 16,16 0 1 1 80,64 A 16,16 0 1 1 48,64 Z')))

# ------------------------------------------------------------- the radar ---
# The ring goes OVER the round minimap (the engine draws the map first, then the
# document): a soft dark rim inside so the map's edge sinks in, the ring itself,
# ticks every 10 degrees and longer ones every 30. White; the HUD tints it.
function Circle([double]$r) { Geo ("M {0},64 A {1},{1} 0 1 1 {2},64 A {1},{1} 0 1 1 {0},64 Z" -f (64 - $r), $r, (64 + $r)) }
$rim = New-Object System.Windows.Media.RadialGradientBrush
$rim.GradientOrigin = New-Object System.Windows.Point(0.5, 0.5)
$rim.Center = New-Object System.Windows.Point(0.5, 0.5)
$rim.RadiusX = 0.5; $rim.RadiusY = 0.5
$rim.GradientStops.Add((New-Object System.Windows.Media.GradientStop([System.Windows.Media.Color]::FromArgb(0, 0, 0, 0), 0.82)))
$rim.GradientStops.Add((New-Object System.Windows.Media.GradientStop([System.Windows.Media.Color]::FromArgb(170, 8, 8, 6), 1.0)))
$ticks = ''
for ($d = 0; $d -lt 360; $d += 10) {
    $a = $d * [Math]::PI / 180
    $r0 = if ($d % 30) { 55.5 } else { 52.5 }
    $ticks += ("M {0:F2},{1:F2} L {2:F2},{3:F2} " -f (64 + $r0 * [Math]::Sin($a)), (64 - $r0 * [Math]::Cos($a)), (64 + 57.5 * [Math]::Sin($a)), (64 - 57.5 * [Math]::Cos($a)))
}
$ring = @(
    @{ g = (Circle 58); brush = $rim },
    @{ g = (Circle 60.5); stroke = 2.2; alpha = 235 },
    @{ g = (Circle 57.5); stroke = 0.7; alpha = 120 },
    @{ g = (Geo $ticks); stroke = 0.9; alpha = 170 }
)
# north: a small wedge sitting on the ring at the top (the HUD turns the whole box)
$north = @(F (Geo 'M 64,1.5 L 69,11 L 64,8.6 L 59,11 Z'))

# ------------------------------------------------------------------ render ---
Render 'map_ring' 512 $false $ring
Render 'map_north' 512 $false $north
Render 'st_radiation' 128 $true $rad
Render 'st_bleeding' 128 $true $blood
Render 'st_hunger' 128 $true $hunger
Render 'st_thirst' 128 $true $thirst
Render 'st_psy' 128 $true $psy
Render 'st_jammed' 128 $true $jam
Render 'st_immortal' 128 $true $god
Render 'st_sleep' 128 $true $sleep
Render 'st_nosleep' 128 $true $nosleep
Render 'st_shelter' 128 $true $shelter
Render 'st_overweight' 128 $true $weight
Render 'gl_noise' 64 $false $noise
Render 'gl_light' 64 $false $light
