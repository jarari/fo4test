@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0..\build\tests" mkdir "%~dp0..\build\tests"
pushd "%~dp0..\build\tests"
cl /nologo /EHsc /MD /std:c++20 /I"%~dp0..\extern\Streamline\external\ngx-sdk\include" /FI"%~dp0nr_test_support.h" "%~dp0nr_backend_policy_regression.cpp" /link dxgi.lib d3d12.lib /OUT:nr-backend-policy-regression.exe
if errorlevel 1 (popd & exit /b 1)
nr-backend-policy-regression.exe
set "testResult=%errorlevel%"
popd
exit /b %testResult%
