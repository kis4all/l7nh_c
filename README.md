# L7NH Servo Drive EtherCAT Controller

This application controls an L7NH servo drive using the SOEM (Simple Open EtherCAT Master) library in torque mode. It provides a Windows GUI with start/stop buttons and RPM display.

## Features
- GUI with start/stop buttons
- Real-time RPM display
- Torque mode control using CiA 402 state machine
- PDO-based real-time communication
- Proper state machine handling for safe operation

## Prerequisites
- Windows 10/11 with Visual Studio 2022 Build Tools
- CMake 3.15 or later
- Ethernet interface connected to EtherCAT network
- SOEM library (included in the SOEM subdirectory)

## Build Instructions

### Method 1: Using build.bat (Recommended)
1. Make sure you have SOEM library in the SOEM subdirectory
2. Run `build.bat` as Administrator to build the application
3. The executable will be in the `build/Release` directory

### Method 2: Manual build
1. Open a Developer Command Prompt for VS 2022
2. Navigate to the project directory
3. Create build directory: `mkdir build && cd build`
4. Configure: `cmake .. -G "Visual Studio 17 2022" -A x64`
5. Build: `cmake --build . --config Release`

## Configuration
- Edit `src/main.c` to change the network interface name from "Ethernet" to match your actual EtherCAT interface name
  - Find the line `const char* ifname = "Ethernet";` and change "Ethernet" to your interface name
  - You can check your interface name using `ipconfig` command
- Adjust torque values in the `StartServo()` function as needed

## Usage
1. Connect your computer to the EtherCAT network with the L7NH servo drive
2. Run the executable as Administrator
3. Press 'Start' to begin torque control (drive will start rotating)
4. Press 'Stop' to stop the servo drive
5. The RPM display shows the actual speed of the motor

## Safety Notes
- Always ensure proper safety measures when controlling servo drives
- Verify correct network configuration before running
- Check all mechanical connections before operation
- The application sets a default torque of 10% when started - adjust as needed

## EtherCAT Network Requirements
- The servo drive should be at position 1 on the EtherCAT network
- Make sure the drive is configured for EtherCAT communication
- Verify the ESI file matches your drive model

## Troubleshooting
- If "No socket connection" error appears, verify the interface name is correct
- If "Drive not ready to start" appears, check the drive's physical state and connections
- If communication errors occur, verify the EtherCAT network topology

## Code Overview
- Main GUI: Implemented using Windows API in src/main.c
- EtherCAT communication: Using SOEM library
- CiA 402 state machine: Proper state transitions for safe operation
- PDO communication: Real-time torque and velocity data exchange
- Torque control: Direct torque control mode (mode 0x04)