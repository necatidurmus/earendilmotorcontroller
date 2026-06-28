Drivers/

This folder is intentionally empty. The actual HAL/LL/CMSIS sources
are pulled in by PlatformIO at build time from the
`framework-stm32cubef4` package (HAL, LL, CMSIS, startup_*.S,
system_stm32f4xx.c).  Adding files here would risk conflicting
with the framework-provided copies, so we leave the folder as a
placeholder for the CubeMX-style directory layout that STM32CubeIDE
expects when importing the project.
