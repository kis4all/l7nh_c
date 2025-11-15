@echo off
echo Running EtherCAT Servo Control Application...

rem Change to build directory
cd build

rem Run the executable
if exist Release\ethercat_servo_control.exe (
    echo Starting application...
    Release\ethercat_servo_control.exe
) else (
    echo Error: Executable not found. Please build the project first.
    echo Run build.bat to build the project.
    pause
)