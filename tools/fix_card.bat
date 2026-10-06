@echo off
echo === 重建 D: 分区 + FAT32 格式化 ===
echo === 请右键本文件 → "以管理员身份运行" ===
echo.
echo 步骤：
echo   1. 先按提示按 Y 确认 select disk 1
echo   2. clean 后再按 Y 确认 create partition primary
echo   3. format 完成后按 Y 确认 assign letter=d
echo   4. exit 退出
echo.
pause
diskpart
