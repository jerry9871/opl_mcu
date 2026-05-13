# Download famous OPL game packs from vgmrips.net and convert to DRO.
# Run from the repo root: powershell -File tools\download_packs.ps1

$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '..')

$tmp = Join-Path "songs" "_dl_tmp"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$packs = @(
    @{ Name="Dune (PC)";               Url="https://vgmrips.net/files/Computers/IBM_PC/Dune_%28IBM_PC_AT%29.zip" },
    @{ Name="MegaRace (PC)";           Url="https://vgmrips.net/files/Computers/IBM_PC/MegaRace_%28PC%29.zip" },
    @{ Name="Tyrian (PC)";             Url="https://vgmrips.net/files/Computers/IBM_PC/Tyrian_%28PC%29.zip" },
    @{ Name="Transport Tycoon";        Url="https://vgmrips.net/files/Computers/IBM_PC/Transport_Tycoon_%28IBM_PC_AT%29.zip" },
    @{ Name="Legend of Kyrandia";      Url="https://vgmrips.net/files/Computers/IBM_PC/The_Legend_of_Kyrandia_-_Book_One_%28IBM_PC_AT%29.zip" },
    @{ Name="Secret of Monkey Island"; Url="https://vgmrips.net/files/Computers/IBM_PC/The_Secret_of_Monkey_Island_%28IBM_PC_AT%29.zip" },
    @{ Name="Heretic";                 Url="https://vgmrips.net/files/Computers/IBM_PC/Heretic_%28IBM_PC_AT%29.zip" },
    @{ Name="Wing Commander";          Url="https://vgmrips.net/files/Computers/IBM_PC/Wing_Commander_%28IBM_PC_AT%29.zip" }
)

$totalOk  = 0
$totalErr = 0

foreach ($pack in $packs) {
    $safeName = $pack.Name -replace '[^A-Za-z0-9]','_'
    $zip = Join-Path $tmp "$safeName.zip"
    $dir = Join-Path $tmp $safeName

    Write-Host "`n=== $($pack.Name) ===" -ForegroundColor Cyan

    if (-not (Test-Path $zip)) {
        Write-Host "  Downloading..."
        Invoke-WebRequest -Uri $pack.Url -OutFile $zip -UseBasicParsing
    } else {
        Write-Host "  (zip already cached)"
    }

    Write-Host "  Extracting..."
    Expand-Archive -Path $zip -DestinationPath $dir -Force

    $vgzFiles = Get-ChildItem $dir -Recurse -Include "*.vgz","*.vgm"
    Write-Host "  $($vgzFiles.Count) tracks found"

    foreach ($vgz in $vgzFiles) {
        # Skip if DRO already exists
        $dro = Join-Path "songs" ($vgz.BaseName + ".dro")
        if (Test-Path $dro) {
            Write-Host "    SKIP (exists) $($vgz.BaseName)" -ForegroundColor DarkGray
            continue
        }
        $result = & .\tools\vgz2dro.exe $vgz.FullName $dro 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host "    OK   $($vgz.BaseName)" -ForegroundColor Green
            $totalOk++
        } else {
            Write-Host "    ERR  $($vgz.BaseName): $result" -ForegroundColor Red
            $totalErr++
        }
    }
}

Write-Host "`nCleaning up temp files..."
Remove-Item -Recurse -Force $tmp

Write-Host "`n--- Summary: $totalOk converted, $totalErr errors ---" -ForegroundColor Cyan
Write-Host "`nRegenerating song headers..."
powershell -File tools\regen_songs.ps1

Pop-Location
