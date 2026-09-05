<#
=============================================================================
 build.ps1 - Compilacion en Windows sin make
=============================================================================

 Por que existe: MinGW se instala muchas veces sin mingw32-make, y pedirle a
 quien corrige que instale make para ver el ejercicio es una friccion evitable.
 Este script busca gcc en el PATH y, si no lo encuentra, en las rutas tipicas.

 Uso:
     .\build.ps1              compila los tres binarios en build\
     .\build.ps1 -Test        compila y corre las pruebas
     .\build.ps1 -Run         compila y corre el banco con el vector del Anexo B
     .\build.ps1 -Salidas     regenera los archivos de ..\salida\
     .\build.ps1 -Clean       borra build\

 Si la politica de ejecucion de PowerShell lo bloquea:
     powershell -ExecutionPolicy Bypass -File .\build.ps1
=============================================================================
#>

param(
    [switch]$Test,
    [switch]$Run,
    [switch]$Salidas,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$raiz    = Split-Path -Parent $MyInvocation.MyCommand.Path
$build   = Join-Path $raiz 'build'
$anexos  = Join-Path (Split-Path -Parent $raiz) 'Anexos'
$salida  = Join-Path (Split-Path -Parent $raiz) 'salida'

# ---- 1) Encontrar gcc -------------------------------------------------------
function Buscar-Gcc {
    $g = Get-Command gcc -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }
    foreach ($p in @(
        'C:\MinGW\bin\gcc.exe',
        'C:\msys64\mingw64\bin\gcc.exe',
        'C:\msys64\ucrt64\bin\gcc.exe',
        'C:\mingw64\bin\gcc.exe',
        'C:\TDM-GCC-64\bin\gcc.exe',
        "$env:ProgramFiles\LLVM\bin\clang.exe")) {
        if (Test-Path $p) { return $p }
    }
    throw "No encontre gcc ni clang. Instala MinGW-w64 o agrega gcc al PATH."
}

$gcc = Buscar-Gcc

if ($Clean) {
    if (Test-Path $build) { Remove-Item -Recurse -Force $build }
    Write-Host "build\ borrado."
    return
}

if (-not (Test-Path $build)) { New-Item -ItemType Directory -Path $build | Out-Null }

# ---- 2) Fuentes -------------------------------------------------------------
$lib   = Get-ChildItem (Join-Path $raiz 'lib\vcp\src\*.c') | ForEach-Object FullName
$port  = Get-ChildItem (Join-Path $raiz 'port\pc\*.c')     | ForEach-Object FullName
$tests = Get-ChildItem (Join-Path $raiz 'tests\*.c')       | ForEach-Object FullName

$flags = @(
    '-std=c11',
    '-Wall','-Wextra','-Wpedantic','-Wshadow','-Wconversion','-Wcast-qual',
    '-Wstrict-prototypes','-Wmissing-prototypes',
    '-O2',
    "-I$(Join-Path $raiz 'lib\vcp\include')",
    "-I$(Join-Path $raiz 'port\pc')",
    "-I$(Join-Path $raiz 'tests')"
)

function Compilar([string]$nombre, [string[]]$fuentes) {
    $exe = Join-Path $build "$nombre.exe"
    Write-Host "  compilando $nombre ..." -NoNewline
    & $gcc @flags @fuentes -o $exe
    if ($LASTEXITCODE -ne 0) { throw "fallo la compilacion de $nombre" }
    Write-Host " ok  -> $exe"
}

Write-Host "gcc: $gcc"
Compilar 'vcp1'       ($lib + $port + @(Join-Path $raiz 'app\main_vcp1.c'))
Compilar 'demo_venta' ($lib + $port + @(Join-Path $raiz 'app\main_demo_venta.c'))
Compilar 'tests'      ($lib + $port + $tests)

# ---- 3) Acciones ------------------------------------------------------------
if ($Test) {
    Write-Host ""
    & (Join-Path $build 'tests.exe')
    if ($LASTEXITCODE -ne 0) { Write-Host "HAY PRUEBAS QUE FALLAN" -ForegroundColor Red }
}

if ($Run) {
    Write-Host ""
    & (Join-Path $build 'vcp1.exe') (Join-Path $anexos 'stream_vcp1.txt')
}

if ($Salidas) {
    if (-not (Test-Path $salida)) { New-Item -ItemType Directory -Path $salida | Out-Null }

    # Se corre desde firmware\ y con ruta RELATIVA al vector, para que el
    # encabezado del informe no quede con la ruta absoluta de esta maquina.
    Push-Location $raiz
    $vector = '..\Anexos\stream_vcp1.txt'

    # UTF8Encoding($false) = sin BOM. Out-File / Set-Content en PowerShell 5.1
    # agregan BOM, que ensucia un archivo de texto que va al repositorio.
    $sinBom = New-Object System.Text.UTF8Encoding $false

    function Guardar([string]$archivo, [string[]]$lineas) {
        [System.IO.File]::WriteAllLines((Join-Path $salida $archivo), $lineas, $sinBom)
    }

    Guardar '01_anexoB_reproceso.txt' (& '.\build\vcp1.exe' $vector)
    Guardar '02_anexoB_descarte.txt'  (& '.\build\vcp1.exe' --resync descarte $vector)
    Guardar '03_anexoB_sin_plazo.txt' (& '.\build\vcp1.exe' --sin-plazo $vector)
    Guardar '04_demo_venta.txt'       (& '.\build\demo_venta.exe')
    Guardar '05_tests.txt'            (& '.\build\tests.exe')

    Pop-Location
    Write-Host ""
    Write-Host "salidas regeneradas en $salida"
}
