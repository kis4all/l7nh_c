@echo off
echo Building EtherCAT Servo Control Application...

rem Create build directory if it doesn't exist
if not exist build mkdir build

rem Change to build directory
cd build

rem Configure with CMake
cmake .. -G "Visual Studio 17 2022" -A x64

rem Build the project
cmake --build . --config Release

echo Build process completed. Check the build/Release directory for the executable.
pause