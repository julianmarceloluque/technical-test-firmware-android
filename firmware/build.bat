@echo off
rem ==========================================================================
rem  build.bat - Compilacion en Windows desde cmd, sin make y sin PowerShell
rem ==========================================================================
rem
rem   build.bat          compila los tres binarios en build\
rem   build.bat test     compila y corre las pruebas
rem   build.bat run      compila y corre el banco con el vector del Anexo B
rem
rem  Si gcc no esta en el PATH, ajustar GCC de abajo.
rem ==========================================================================

setlocal

set GCC=gcc
where %GCC% >nul 2>nul || set GCC=C:\MinGW\bin\gcc.exe
if not exist "%GCC%" (
  where gcc >nul 2>nul || (
    echo No encontre gcc. Instala MinGW-w64 o edita GCC en este archivo.
    exit /b 1
  )
)

set RAIZ=%~dp0
set INC=-I"%RAIZ%lib\vcp\include" -I"%RAIZ%port\pc" -I"%RAIZ%tests"
set FLAGS=-std=c11 -Wall -Wextra -Wpedantic -Wshadow -O2 %INC%
set LIB="%RAIZ%lib\vcp\src\*.c" "%RAIZ%port\pc\*.c"

if not exist "%RAIZ%build" mkdir "%RAIZ%build"

echo compilando vcp1 ...
%GCC% %FLAGS% %LIB% "%RAIZ%app\main_vcp1.c" -o "%RAIZ%build\vcp1.exe" || exit /b 1

echo compilando demo_venta ...
%GCC% %FLAGS% %LIB% "%RAIZ%app\main_demo_venta.c" -o "%RAIZ%build\demo_venta.exe" || exit /b 1

echo compilando tests ...
%GCC% %FLAGS% %LIB% "%RAIZ%tests\*.c" -o "%RAIZ%build\tests.exe" || exit /b 1

echo listo.

if /I "%1"=="test" "%RAIZ%build\tests.exe"
if /I "%1"=="run"  "%RAIZ%build\vcp1.exe" "%RAIZ%..\Anexos\stream_vcp1.txt"

endlocal
