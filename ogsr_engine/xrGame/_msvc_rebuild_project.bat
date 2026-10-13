@echo off
rem Usage (from Git Bash): cmd //c "D:\OGSR-ONLINE\ogsr_engine\xrGame\_msvc_rebuild_project.bat" ogsr_engine/xrGame/xrGame.vcxproj xrgame
rem Rebuild ONE project only (no solution-level Rebuild: that also rebuilds LuaJIT, whose
rem NMakeReBuildCommandLine does RD /S/Q ..\bin and then fails under bare msbuild).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "VULKAN_SDK=C:\VulkanSDK\1.4.350.0"
cd /d D:\OGSR-ONLINE
msbuild %1 /t:Rebuild -p:Configuration=Release -p:Platform=x64 -p:SolutionDir=D:\OGSR-ONLINE\ -p:BuildProjectReferences=false -m -v:minimal -nologo > "%~dp0_msvc_%2.log" 2>&1
echo BUILD_EXIT_CODE=%ERRORLEVEL% >> "%~dp0_msvc_%2.log"
