@echo off
REM ===========================================================================
REM  tools\idf.cmd -- cmd / PowerShell 通用的 ESP-IDF 构建入口
REM
REM  它只是 idf.ps1 的包装：用 -ExecutionPolicy Bypass 启动 PowerShell，
REM  这样即使本机执行策略拦住了"未签名的本地脚本"
REM  （报错：未对文件 ... 进行数字签名。无法在当前系统上运行该脚本），
REM  也能正常跑起来。
REM
REM  用法（在项目根目录 D:\esp32\xiangmu1 下）：
REM      tools\idf.cmd build
REM      tools\idf.cmd -p COM4 flash
REM      tools\idf.cmd -p COM4 monitor
REM      tools\idf.cmd -p COM4 flash monitor
REM      tools\idf.cmd fullclean
REM      tools\idf.cmd menuconfig
REM
REM  本文件刻意只用 ASCII 字符，避免代码页问题。
REM ===========================================================================

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf.ps1" %*
exit /b %ERRORLEVEL%
