# icon.exe testleri - sonuclar test.log dosyasina yazilir
$ErrorActionPreference = 'Continue'
Set-Location -LiteralPath $PSScriptRoot
$log = Join-Path $PSScriptRoot 'test.log'
Set-Content -LiteralPath $log -Value ("test basladi " + (Get-Date)) -Encoding UTF8
function L($m) { Add-Content -LiteralPath $log -Value $m -Encoding UTF8 }
$exe = Join-Path $PSScriptRoot 'icon.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $PSScriptRoot 'bin\icon.exe' }
$fail = 0

function ReadIco($path) {
  $b = [System.IO.File]::ReadAllBytes($path)
  $count = [BitConverter]::ToUInt16($b, 4)
  $list = @()
  for ($i = 0; $i -lt $count; $i++) {
    $o = 6 + 16 * $i
    $w = $b[$o]; if ($w -eq 0) { $w = 256 }
    $bpp = [BitConverter]::ToUInt16($b, $o + 6)
    $len = [BitConverter]::ToUInt32($b, $o + 8)
    $off = [BitConverter]::ToUInt32($b, $o + 12)
    $list += ,@($w, $bpp, $len, $off)
  }
  return @{ bytes = $b; entries = $list }
}

function CheckIco($path, $label) {
  if (-not (Test-Path -LiteralPath $path)) { L "FAIL $label : ICO yok ($path)"; return $false }
  $r = ReadIco $path
  $e = $r.entries
  $desc = ($e | ForEach-Object { "$($_[0])x$($_[1])bit" }) -join ' '
  L "$label : $desc"
  $want = @('16x32bit','16x8bit','24x32bit','24x8bit','32x32bit','32x8bit','48x32bit','48x8bit','64x32bit','64x8bit','128x32bit','128x8bit','256x32bit')
  $got = $e | ForEach-Object { "$($_[0])x$($_[1])bit" }
  foreach ($w in $want) { if ($got -notcontains $w) { L "FAIL $label : eksik giris $w"; return $false } }
  if ($got.Count -ne $want.Count) { L "FAIL $label : beklenmeyen giris sayisi $($got.Count)"; return $false }
  # 256 giris PNG imzasiyla baslamali
  $big = $e | Where-Object { $_[0] -eq 256 } | Select-Object -First 1
  $sig = $r.bytes[$big[3]..($big[3] + 3)]
  if (-not ($sig[0] -eq 0x89 -and $sig[1] -eq 0x50 -and $sig[2] -eq 0x4E -and $sig[3] -eq 0x47)) { L "FAIL $label : 256 giris PNG degil"; return $false }
  # dosya sinirlari
  foreach ($x in $e) { if (($x[3] + $x[2]) -gt $r.bytes.Length) { L "FAIL $label : giris dosya disina tasiyor"; return $false } }
  # System.Drawing ile ac (Windows'un kendi ICO ayristiricisi)
  try {
    Add-Type -AssemblyName System.Drawing
    $ic = New-Object System.Drawing.Icon($path, 256, 256)
    L "  System.Drawing 256x256 acildi: $($ic.Width)x$($ic.Height)"
    $ic16 = New-Object System.Drawing.Icon($path, 16, 16)
    L "  System.Drawing 16x16 acildi: $($ic16.Width)x$($ic16.Height)"
    $bmp = $ic.ToBitmap(); $px = $bmp.GetPixel(0,0); L "  256 sol-ust piksel alfa = $($px.A) (0 beklenir)"
    if ($px.A -ne 0) { L "FAIL $label : kose seffaf degil"; return $false }
    $bmp.Dispose(); $ic.Dispose(); $ic16.Dispose()
  } catch { L "  (bilgi) System.Drawing bu girisi acamadi: $($_.Exception.Message)" }
  return $true
}

function Run($label, $argList, $expectExit) {
  $so = Join-Path $PSScriptRoot 'stdout.tmp'
  if (Test-Path $so) { Remove-Item $so -Force }
  $p = Start-Process -FilePath $exe -ArgumentList $argList -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $so
  $txt = ''
  if (Test-Path $so) { $txt = (Get-Content -LiteralPath $so -Raw -Encoding UTF8) }
  L "[$label] exit=$($p.ExitCode) (beklenen $expectExit)"
  if ($txt) { L ("  cikti: " + ($txt -replace "\r?\n", " | ")) }
  if ($p.ExitCode -ne $expectExit) { L "FAIL $label : cikis kodu"; $script:fail++ }
  return $txt
}

$out = Join-Path $PSScriptRoot 'out'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null

# 1) -s ile SVG, -o ile klasor
Run '1 -s svg -o' "-s `"samples\mp3-icon-v2.svg`" -o `"$out`"" 0 | Out-Null
if (-not (CheckIco (Join-Path $out 'mp3-icon-v2.ico') '1')) { $fail++ }

# 2) -p ile PNG, Turkce karakterli klasor (olmayan klasor olusturulmali)
$tr = Join-Path $out ([string]::Concat('t', [char]0xFC, 'rk', [char]0xE7, 'e ', [char]0x015F, 'ablon'))
Run '2 -p png -o unicode' "-p `"samples\mp3-icon-512.png`" -o `"$tr`"" 0 | Out-Null
if (-not (CheckIco (Join-Path $tr 'mp3-icon-512.ico') '2')) { $fail++ }

# 3) -o yok: girdinin klasorune yazar
$work = Join-Path $out 'work'; New-Item -ItemType Directory -Path $work | Out-Null
Copy-Item 'samples\mp3-icon.svg' $work
Run '3 -s svg (cikti klasoru yok)' "-s `"$work\mp3-icon.svg`"" 0 | Out-Null
if (-not (CheckIco (Join-Path $work 'mp3-icon.ico') '3')) { $fail++ }

# 4) Surukle-birak bicimi: bayraksiz, birden cok dosya
Copy-Item 'samples\mp3-icon-512.png' $work
Remove-Item (Join-Path $work 'mp3-icon.ico') -Force
Run '4 bayraksiz coklu dosya' "`"$work\mp3-icon.svg`" `"$work\mp3-icon-512.png`"" 0 | Out-Null
if (-not (CheckIco (Join-Path $work 'mp3-icon.ico') '4a')) { $fail++ }
if (-not (CheckIco (Join-Path $work 'mp3-icon-512.ico') '4b')) { $fail++ }

# 5) Hata durumlari (cikis kodu 1 ve hata metni)
$t = Run '5a olmayan dosya' "-p `"$out\yok.png`"" 1
if ($t -notmatch 'bulunamad') { L "FAIL 5a : hata metni beklenenden farkli"; $fail++ }
$t = Run '5b svg dosyasi -p ile' "-p `"samples\mp3-icon.svg`"" 1
if ($t -notmatch 'PNG') { L "FAIL 5b : hata metni beklenenden farkli"; $fail++ }
$t = Run '5c bilinmeyen secenek' "-x" 1
if ($t -notmatch 'Bilinmeyen') { L "FAIL 5c : hata metni beklenenden farkli"; $fail++ }
$t = Run '5d -o degersiz' "-p samples\mp3-icon-512.png -o" 1
if ($t -notmatch 'de.er verilmedi') { L "FAIL 5d : hata metni beklenenden farkli"; $fail++ }
Set-Content -LiteralPath (Join-Path $work 'bozuk.png') -Value 'bu png degil' -Encoding ASCII
$t = Run '5e bozuk dosya' "`"$work\bozuk.png`"" 1
if ($t -notmatch 'desteklenmeyen') { L "FAIL 5e : hata metni beklenenden farkli"; $fail++ }

# 6) Yardim
$t = Run '6 -h' "-h" 0
if ($t -notmatch 'Kullan') { L "FAIL 6 : yardim metni yok"; $fail++ }

if ($fail -eq 0) { L 'ALL_TESTS_PASSED' } else { L "TESTS_FAILED ($fail)" }
