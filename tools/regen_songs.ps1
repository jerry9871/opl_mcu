# Regenerate songs/*_song.h from songs/*.dro and refresh songs/song_index.h.
#
# Usage:  powershell -File tools/regen_songs.ps1

$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '..')

$total_pl = 0
$total_hs = 0
$count    = 0

Get-ChildItem songs\*.dro | ForEach-Object {
    $base = $_.BaseName
    $sym  = ($base -replace '[^A-Za-z0-9]','_').ToLower()
    if ($sym -match '^[0-9]') { $sym = 's_' + $sym }
    $hdr  = "songs\${sym}_song.h"

    $out = .\tools\dro2hdr.exe $_.FullName $hdr $sym --hs 2>&1 | Select-Object -Last 1

    if ($out -match '(\d+) bytes plain -> (\d+) bytes hs') {
        $total_pl += [int]$Matches[1]
        $total_hs += [int]$Matches[2]
        $count++
    }
    "{0,-50} {1}" -f $base, $out
}

"---"
"{0} songs, {1} -> {2} bytes ({3:N1}% saved)" -f `
    $count, $total_pl, $total_hs, (100 * ($total_pl - $total_hs) / $total_pl)

Pop-Location
