@echo off
rem Converts the extracted APK data (tools\merged, see extract_apk.py) into the
rem game's romfs\*.pak files.
cd /d "%~dp0"
if not exist ..\game\romfs mkdir ..\game\romfs
set PYTHONIOENCODING=utf-8
for %%t in (make_data.py make_hud.py make_ui.py make_char.py make_nav.py make_trees.py) do (
    echo == %%t
    python %%t || exit /b 1
)
echo done: ..\game\romfs
