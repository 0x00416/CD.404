.DEFAULT_GOAL := all

SHELL := cmd.exe
.SHELLFLAGS := /d /s /c
.SUFFIXES:

CMAKE ?= cmake
CTEST ?= ctest
VSWHERE ?= C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe
VS_INSTALLATION := $(strip $(shell "$(VSWHERE)" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>NUL))
VSDEVCMD ?= $(VS_INSTALLATION)/Common7/Tools/VsDevCmd.bat
MSVC_CODE_PAGE ?= $(strip $(shell powershell.exe -NoProfile -Command "[Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage"))

DEBUG_CONFIGURE_PRESET := ninja-msvc-x64
DEBUG_BUILD_PRESET := ninja-msvc-debug
RELEASE_CONFIGURE_PRESET := ninja-msvc-x64-release
RELEASE_BUILD_PRESET := ninja-msvc-release

DEBUG_APP := out/build/$(DEBUG_CONFIGURE_PRESET)/apps/cd404/CD.404.exe
RELEASE_APP := out/build/$(RELEASE_CONFIGURE_PRESET)/apps/cd404/CD.404.exe

.PHONY: all check configure debug test release test-release run run-release clean help

all: debug test

check:
	@if not exist "$(VSDEVCMD)" (echo [CD.404] ERROR: Visual Studio C++ Build Tools not found. Install MSVC and the Windows SDK, or pass VSDEVCMD=path/to/VsDevCmd.bat. & exit /b 1)
	@"$(CMAKE)" --version >NUL 2>&1 || (echo [CD.404] ERROR: cmake is not available. Add it to PATH or pass CMAKE=path/to/cmake.exe. & exit /b 1)
	@"$(CTEST)" --version >NUL 2>&1 || (echo [CD.404] ERROR: ctest is not available. Add it to PATH or pass CTEST=path/to/ctest.exe. & exit /b 1)

configure: check
	@echo [CD.404] Configuring Debug with Ninja and MSVC...
	@call "$(VSDEVCMD)" -no_logo -arch=x64 -host_arch=x64 >NUL && chcp $(MSVC_CODE_PAGE) >NUL && set "VSLANG=1033" && "$(CMAKE)" --preset $(DEBUG_CONFIGURE_PRESET)

debug: configure
	@echo [CD.404] Building the complete Debug application...
	@call "$(VSDEVCMD)" -no_logo -arch=x64 -host_arch=x64 >NUL && chcp $(MSVC_CODE_PAGE) >NUL && set "VSLANG=1033" && "$(CMAKE)" --build --preset $(DEBUG_BUILD_PRESET)

test: debug
	@echo [CD.404] Running Debug tests...
	@"$(CTEST)" --preset $(DEBUG_BUILD_PRESET) --output-on-failure

release: check
	@echo [CD.404] Configuring Release with Ninja and MSVC...
	@call "$(VSDEVCMD)" -no_logo -arch=x64 -host_arch=x64 >NUL && chcp $(MSVC_CODE_PAGE) >NUL && set "VSLANG=1033" && "$(CMAKE)" --preset $(RELEASE_CONFIGURE_PRESET)
	@echo [CD.404] Building the complete Release application...
	@call "$(VSDEVCMD)" -no_logo -arch=x64 -host_arch=x64 >NUL && chcp $(MSVC_CODE_PAGE) >NUL && set "VSLANG=1033" && "$(CMAKE)" --build --preset $(RELEASE_BUILD_PRESET)
	@echo [CD.404] Running Release tests...
	@"$(CTEST)" --preset $(RELEASE_BUILD_PRESET) --output-on-failure

test-release: release

run: debug
	@echo [CD.404] Starting $(DEBUG_APP)...
	@start "CD.404" "$(DEBUG_APP)"

run-release: release
	@echo [CD.404] Starting $(RELEASE_APP)...
	@start "CD.404" "$(RELEASE_APP)"

clean:
	@"$(CMAKE)" --version >NUL 2>&1 || (echo [CD.404] ERROR: cmake is not available. Add it to PATH or pass CMAKE=path/to/cmake.exe. & exit /b 1)
	@echo [CD.404] Removing all generated output and legacy build artifacts...
	@if exist "out" "$(CMAKE)" -E remove_directory "out"
	@if exist "build" "$(CMAKE)" -E remove_directory "build"
	@if exist "windows_sdk_probe.obj" "$(CMAKE)" -E rm -f "windows_sdk_probe.obj"

help:
	@echo CD.404 build targets:
	@echo   make              Configure, build, and test Debug
	@echo   make debug        Configure and build Debug
	@echo   make test         Configure, build, and test Debug
	@echo   make release      Configure, build, and test Release
	@echo   make run          Build and start Debug
	@echo   make run-release  Build, test, and start Release
	@echo   make clean        Remove out/, legacy build/, and probe artifacts
	@echo.
	@echo Override tool discovery when needed:
	@echo   make CMAKE=C:/path/cmake.exe CTEST=C:/path/ctest.exe
	@echo   make VSDEVCMD=C:/path/Common7/Tools/VsDevCmd.bat
	@echo   make MSVC_CODE_PAGE=936
