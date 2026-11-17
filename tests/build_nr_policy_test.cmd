@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0..\build\tests" mkdir "%~dp0..\build\tests"
pushd "%~dp0..\build\tests"
cl /nologo /EHsc /W4 /std:c++20 "%~dp0nr_policy_regression.cpp" /Fe:nr-policy-regression.exe
if errorlevel 1 (popd & exit /b 1)
nr-policy-regression.exe
set "testResult=%errorlevel%"
popd
exit /b %testResult%
