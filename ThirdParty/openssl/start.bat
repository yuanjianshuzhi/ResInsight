@echo off
@set PATH=%PATH%;%~dp0bin
@chcp 65001 > nul

title Win64 OpenSSL Command Prompt
echo Win64 OpenSSL Command Prompt
echo.
openssl version -a
echo.

%SystemDrive%
cd %UserProfile%

cmd.exe /K
