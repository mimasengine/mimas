# Copie les fichiers .bin et .cue de Mimas vers D:\02 (écrase sans confirmation)
$ErrorActionPreference = 'Stop'

$src = 'C:\Users\pcico\Projects\Mimas\build'
$dst = 'D:\02'

if (-not (Test-Path -LiteralPath $dst)) {
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
}

$files = Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Extension -in '.bin', '.cue' }

if (-not $files) {
    Write-Warning "Aucun fichier .bin ou .cue trouve dans $src"
    return
}

foreach ($f in $files) {
    Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
    $date = $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
    Write-Host "Copie : $($f.Name) ($date) -> $dst"
}

Write-Host "Termine ($($files.Count) fichier(s))." -ForegroundColor Green
