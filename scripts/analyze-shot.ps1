# analyze-shot.ps1 - heuristic render verification for a captured window PNG
param([Parameter(Mandatory=$true)][string]$Png)
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($Png)
$w = $bmp.Width; $h = $bmp.Height

function RegionStats([System.Drawing.Bitmap]$b, [int]$x0,[int]$y0,[int]$x1,[int]$y1) {
    $n=0; $sum=0; $dark=0
    for($y=$y0; $y -lt $y1; $y+=2){
        for($x=$x0; $x -lt $x1; $x+=2){
            $c = $b.GetPixel($x,$y)
            $lum = [int](0.299*$c.R + 0.587*$c.G + 0.114*$c.B)
            $sum += $lum; $n++
            if ($lum -lt 128) { $dark++ }
        }
    }
    return @{ mean = [math]::Round($sum/[math]::Max(1,$n),1); darkPx = $dark; total = $n }
}

$tabStrip   = RegionStats $bmp ([int]($w*0.05)) 4 ([int]($w*0.95)) ([int]($h*0.06))
$editorBody = RegionStats $bmp ([int]($w*0.25)) ([int]($h*0.35)) ([int]($w*0.75)) ([int]($h*0.65))
$textArea   = RegionStats $bmp 70 ([int]($h*0.15)) ([int]($w*0.7)) ([int]($h*0.75))
$statusBar  = RegionStats $bmp ([int]($w*0.05)) ([int]($h*0.96)) ([int]($w*0.95)) ([int]($h*0.995))

Write-Output ("SIZE {0}x{1}" -f $w,$h)
Write-Output ("TABSTRIP  mean={0} dark={1}/{2}" -f $tabStrip.mean,$tabStrip.darkPx,$tabStrip.total)
Write-Output ("EDITOR    mean={0} dark={1}/{2}" -f $editorBody.mean,$editorBody.darkPx,$editorBody.total)
Write-Output ("TEXTAREA  mean={0} dark={1}/{2}" -f $textArea.mean,$textArea.darkPx,$textArea.total)
Write-Output ("STATUSBAR mean={0} dark={1}/{2}" -f $statusBar.mean,$statusBar.darkPx,$statusBar.total)

$bmp.Dispose()
