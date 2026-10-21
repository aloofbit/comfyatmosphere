<#
.SYNOPSIS
  Two screenshots compared (2026-10-21): the mean difference per channel, the share of pixels that differ by more
  than -Threshold, and an image of the difference (x4) beside them, written to -Out.
#>
param(
    [Parameter(Mandatory)][string]$A,
    [Parameter(Mandatory)][string]$B,
    [string]$Out = '',
    [int]$Threshold = 24
)
Add-Type -AssemblyName System.Drawing
$ia = [System.Drawing.Bitmap]::FromFile((Resolve-Path $A))
$ib = [System.Drawing.Bitmap]::FromFile((Resolve-Path $B))
$w = [Math]::Min($ia.Width, $ib.Width); $h = [Math]::Min($ia.Height, $ib.Height)
$rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
$fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
$da = $ia.LockBits($rect, 'ReadOnly', $fmt); $db = $ib.LockBits($rect, 'ReadOnly', $fmt)
$na = New-Object byte[] ($da.Stride * $h); $nb = New-Object byte[] ($db.Stride * $h)
[Runtime.InteropServices.Marshal]::Copy($da.Scan0, $na, 0, $na.Length)
[Runtime.InteropServices.Marshal]::Copy($db.Scan0, $nb, 0, $nb.Length)
$ia.UnlockBits($da); $ib.UnlockBits($db)
$code = @"
public static class ShotDiff {
    public static double[] Run(byte[] a, byte[] b, byte[] d, int stride, int w, int h, int thr) {
        double s0 = 0, s1 = 0, s2 = 0; long over = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int i = y * stride + x * 4;
            int e0 = System.Math.Abs(a[i] - b[i]), e1 = System.Math.Abs(a[i+1] - b[i+1]), e2 = System.Math.Abs(a[i+2] - b[i+2]);
            s0 += e0; s1 += e1; s2 += e2;
            if (e0 > thr || e1 > thr || e2 > thr) ++over;
            d[i] = (byte)System.Math.Min(255, e0 * 4); d[i+1] = (byte)System.Math.Min(255, e1 * 4); d[i+2] = (byte)System.Math.Min(255, e2 * 4); d[i+3] = 255;
        }
        double n = (double)w * h;
        return new double[] { s2 / n, s1 / n, s0 / n, 100.0 * over / n };
    }
}
"@
if (-not ('ShotDiff' -as [type])) { Add-Type -TypeDefinition $code }
$nd = New-Object byte[] ($da.Stride * $h)
$r = [ShotDiff]::Run($na, $nb, $nd, $da.Stride, $w, $h, $Threshold)
"{0}: mean difference R {1:N2} G {2:N2} B {3:N2}; {4:N2}% of pixels differ by more than {5}" -f (Split-Path $B -Leaf), $r[0], $r[1], $r[2], $r[3], $Threshold
if ($Out) {
    $dimg = New-Object System.Drawing.Bitmap $w, $h, $fmt
    $dd = $dimg.LockBits($rect, 'WriteOnly', $fmt)
    [Runtime.InteropServices.Marshal]::Copy($nd, 0, $dd.Scan0, $nd.Length)
    $dimg.UnlockBits($dd)
    $sheet = New-Object System.Drawing.Bitmap ($w * 3), $h
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.DrawImage($ia, 0, 0, $w, $h); $g.DrawImage($ib, $w, 0, $w, $h); $g.DrawImage($dimg, 2 * $w, 0, $w, $h)
    $g.Dispose()
    $sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Jpeg)
    $dimg.Dispose(); $sheet.Dispose()
}
$ia.Dispose(); $ib.Dispose()
