# make-ico.ps1 - convert a PNG into a multi-size Windows .ico
# (256px stored as PNG payload; 64/48/32/16 as BMP frames).
param(
    [Parameter(Mandatory=$true)][string]$PngPath,
    [Parameter(Mandatory=$true)][string]$IcoPath
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile($PngPath)

function Get-BmpData([System.Drawing.Image]$img,[int]$size){
    $bmp = New-Object System.Drawing.Bitmap($size,$size)
    $g=[System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode=[System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode=[System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    # aspect-fit: never distort non-square sources
    $ratio=[Math]::Min($size/[double]$img.Width,$size/[double]$img.Height)
    $dw=[int]([Math]::Max(1,[Math]::Round($img.Width*$ratio)))
    $dh=[int]([Math]::Max(1,[Math]::Round($img.Height*$ratio)))
    $dx=[int](($size-$dw)/2); $dy=[int](($size-$dh)/2)
    $g.DrawImage($img,$dx,$dy,$dw,$dh)
    $g.Dispose()
    # BGRA bottom-up rows + AND mask
    $rect=New-Object System.Drawing.Rectangle(0,0,$size,$size)
    $bd=$bmp.LockBits($rect,[System.Drawing.Imaging.ImageLockMode]::ReadOnly,[System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride=$bd.Stride
    $xor=New-Object byte[] ($stride*$size)
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0,$xor,0,$stride*$size)
    $bmp.UnlockBits($bd)
    $rows=New-Object System.Collections.Generic.List[byte]
    for($y=$size-1;$y -ge 0;$y--){
        $rowStart=$y*$stride
        for($x=0;$x -lt $size;$x++){
            $i=$rowStart+$x*4
            $rows.Add($xor[$i+2]); $rows.Add($xor[$i+1]); $rows.Add($xor[$i]); $rows.Add($xor[$i+3])
        }
    }
    $maskStride=[Math]::Ceiling($size/8.0); if(($maskStride%4) -ne 0){$maskStride=$maskStride+(4-($maskStride%4))}
    $and=New-Object byte[] ($maskStride*$size)   # all zero = use alpha
    $bmp.Dispose()
    return @($rows.ToArray(),$and)
}

$sizes=@(16,32,48,64,256)
$entries=@()
$dataParts=@()
$offset=6+16*$sizes.Count
foreach($s in $sizes){
    if($s -eq 256){
        $ms=New-Object System.IO.MemoryStream
        $src.Save($ms,[System.Drawing.Imaging.ImageFormat]::Png)
        $png=$ms.ToArray(); $ms.Dispose()
        $entries+=[pscustomobject]@{w=0;h=0;bytes=$png}   # 0 means 256
    } else {
        $pair=Get-BmpData $src $s
        $bmpHdr=New-Object byte[] 40
        $bw=[BitConverter]::GetBytes(40);      [Array]::Copy($bw,0,$bmpHdr,0,4)
        $bw=[BitConverter]::GetBytes([int]$s); [Array]::Copy($bw,0,$bmpHdr,4,4)
        $bw=[BitConverter]::GetBytes([int]$s); [Array]::Copy($bw,0,$bmpHdr,8,4)
        $bmpHdr[12]=1; $bmpHdr[14]=32                          # planes, bpp
        $frame=$bmpHdr+$pair[0]+$pair[1]
        $entries+=[pscustomobject]@{w=$s;h=$s;bytes=$frame}
    }
}
$src.Dispose()

$msOut=New-Object System.IO.MemoryStream
$bw2={param($b) $msOut.Write($b,0,$b.Length)}
# ICONDIR
$header=New-Object byte[] 6
$header[0]=0;$header[1]=0;$header[2]=1;$header[3]=0
$c=[BitConverter]::GetBytes([uint16]$sizes.Count); $header[4]=$c[0];$header[5]=$c[1]
&$bw2 $header
for($i=0;$i -lt $entries.Count;$i++){
    $e=$entries[$i]; $len=$e.bytes.Length
    $e8=New-Object byte[] 16
    $e8[0]=if($e.w -eq 0){0}else{$e.w}; $e8[1]=if($e.h -eq 0){0}else{$e.h}
    $e8[2]=0;$e8[3]=0                                   # palette
    $c=[BitConverter]::GetBytes([uint16]1);$e8[4]=$c[0];$e8[5]=$c[1]   # plane
    $c=[BitConverter]::GetBytes([uint16]32);$e8[6]=$c[0];$e8[7]=$c[1]  # bpp
    $cl=[BitConverter]::GetBytes([uint32]$len);$e8[8]=$cl[0];$e8[9]=$cl[1];$e8[10]=$cl[2];$e8[11]=$cl[3]
    $co=[BitConverter]::GetBytes([uint32]$offset);$e8[12]=$co[0];$e8[13]=$co[1];$e8[14]=$co[2];$e8[15]=$co[3]
    &$bw2 $e8
}
foreach($e in $entries){ &$bw2 $e.bytes }
[System.IO.File]::WriteAllBytes($IcoPath,$msOut.ToArray())
"ICO written: $IcoPath ($((Get-Item $IcoPath).Length) bytes)"
