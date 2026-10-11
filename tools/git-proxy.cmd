@echo off
REM ===========================================================================
REM  tools\git-proxy.cmd
REM
REM  作用：让 git 的代理设置跟系统代理（Clash 之类）的开/关保持一致。
REM
REM  为什么需要它：
REM    浏览器 / VS Code / PowerShell 会自动读系统的 WinINET 代理设置，
REM    但 git.exe 和 curl.exe 不会。于是会出现两种相反的故障：
REM      1) 代理开着：git 直连 github.com 超时
REM         Failed to connect to github.com port 443 ...
REM      2) 代理关着、而 git 里还留着 http.proxy：
REM         Failed to connect to 127.0.0.1 port 7877 ...
REM    两种情况的修法正好相反，所以不要手改，跑这个脚本让它自己判断。
REM
REM  用法（在项目根目录或任意位置都行）：
REM      tools\git-proxy.cmd
REM ===========================================================================

setlocal
pushd "%~dp0.."

set "PE="
set "PS="

for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings" /v ProxyEnable 2^>nul') do set "PE=%%b"
for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings" /v ProxyServer 2^>nul') do set "PS=%%b"

where git >nul 2>nul
if errorlevel 1 (
    echo [X] PATH 里找不到 git。用 scoop 装的 git 请先执行： scoop reset git
    goto :finish
)

if not defined PS (
    echo [!] 读不到系统代理地址，按"直连"处理。
    git config --local --unset http.proxy  2>nul
    git config --local --unset https.proxy 2>nul
    echo [+] git 已改为直连 GitHub。
    goto :finish
)

if /i "%PE%"=="0x1" (
    git config --local http.proxy  http://%PS%
    git config --local https.proxy http://%PS%
    echo [+] 系统代理已开启，git 设为走 %PS%
) else (
    git config --local --unset http.proxy  2>nul
    git config --local --unset https.proxy 2>nul
    echo [+] 系统代理已关闭，git 改为直连。
)

echo.
echo 验证命令：  git ls-remote origin
echo （要让所有仓库都生效，把上面 git config 的 --local 换成 --global 再跑一次）
:finish
popd
endlocal
