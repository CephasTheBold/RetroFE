<h1 style="
  display: inline-block !important;
  font-size: 20rem;
">
  <img
    src="./Package/Environment/Common/RetroFE.png"
    alt="Icon"
    height="140px"
    style="
      display: inline-block !important;
      height: 3.5rem;
      margin-right: 1rem;
    "
  />
  <span style="position: relative; bottom: 0.7rem;">
    RetroFE
  </span>
</h1>

[Project Discord](https://discord.gg/dpcsP8Hm9W) | [GitHub Wiki](https://github.com/CoinOPS-Official/RetroFE/tree/master/docs) | [Changelog](CHANGELOG.md)

RetroFE is a cross-platform desktop frontend designed for MAME cabinets and game centers, with a focus on simplicity and customization. 
This repository is actively maintained and hundreds of commits ahead of the original RetroFE project. 
It is designed for use within CoinOPS builds, bringing with it a significant increase in performance, optimisations, and available feature set. 

It's licensed under the terms of the GNU General Public License, version 3 or later (GPLv3).

## What's so special about this fork?
* Performance and optimisations
	* 64-bit codebase
    * C++17 as standard
	* Modern render engine; DX11 for Windows, Metal for MacOS
 	* Hardware accelerated video support for Windows
	* VSync and support for high refresh rate
	* Metadata database build time reduced
	* File caching to prevent drive lashing
	* RAM usage reduced by 70%
* Features
	* Ability to start on random item; fed up of seeing the same game every time?
 	* Robust video marquee and 2nd screen support	 
	* Upgraded attract mode
	* Upgraded favouriting system; global and local favourites
	* Start and exit scripts; run programs such as steam at retrofe launch
	* In depth logging system; 7 logging levels
	* Kiosk mode; lock things down for kids or cleanliness
 	* Local Hiscores integration with hi2txt   
	* And much more!

## System Requirements
* OS
    * Windows (10 or higher)
    * Linux (AppImage requires libc 2.38 or higher)
    * macOS (11 Big Sur or higher)
	* Unix-like systems other than Linux are not officially supported but may work
* Processor
    * A modern CPU (2014 or later) is highly recommended
* Graphics
    * A reasonably modern graphics card (Direct3D 11+ / OpenGL 4+ / Metal on MacOS)

#   Building for Windows #
### Install Requirements

	winget install -e --id Microsoft.VisualStudio.2022.Community
	winget install -e --id Microsoft.WindowsSDK.10.0.26100
	winget install -e --id Microsoft.DotNet.Framework.DeveloperPack_4
	winget install -e --id Kitware.CMake
	winget install -e --id Git.Git

* Open the Visual Studio Installer, modify the install and add the "Desktop development with C++" package group

* Install gstreamer-runtime and gstreamer-devel MSVC 64-bit complete (https://gstreamer.freedesktop.org/download/#windows)

Python 3 - Optional - Read below

  	winget install -e --id Python.Python.3.11

Alternatively, manually install
  
* Visual Studio Community (https://visualstudio.microsoft.com/downloads)
* Microsoft Windows SDK and .NET Framework 4 for Windows 10 and higher (https://developer.microsoft.com/windows/downloads/windows-sdk)
* CMake (https://cmake.org/download)
* Git (https://git-scm.com/downloads/win)
* Python 3 (https://www.python.org/downloads/windows)
* gstreamer-runtime and gstreamer-devel MSVC 64-bit complete (https://gstreamer.freedesktop.org/download/#windows)

### Download and compile the source code
Download the source code

	git clone https://github.com/CoinOPS-Official/RetroFE.git

Setup Environment (to setup necessary variables and paths to compile in visual studio)

	cd RetroFE

Gather submodule for DLLs

 	git submodule update --init --recursive

Generate visual studio solution files

	cmake -A x64 -B .\RetroFE\Build -D GSTREAMER_ROOT=C:\gstreamer\1.0\msvc_x86_64 -S .\RetroFE\Source
  
Compile RetroFE

	cmake --build RetroFE/Build --config Release

Copy in DLLs

	mkdir .\RetroFE\Build\Release\retrofe
	xcopy /S /I /Y .\Package\Environment\Windows\retrofe .\RetroFE\Build\Release\retrofe
	move .\RetroFE\Build\Release\retrofe.exe .\RetroFE\Build\Release\retrofe\retrofe.exe
	copy .\Package\Environment\Windows\RetroFE.lnk .\RetroFE\Build\Release

The executable is then found in `/RetroFE/Build`, copy `RetroFE.lnk` and the `retrofe` folder

#   Building for Linux #

### Install libraries

 #### Debian
```bash
sudo apt-get install git g++ cmake zlib1g-dev \
libsdl3-0 libsdl3-image-0 libsdl3-mixer-0 libsdl3-ttf-0 \
libsdl3-dev libsdl3-image-dev libsdl3-mixer-dev libsdl3-ttf-dev \
libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-good1.0-dev gstreamer1.0-libav \
libglib2.0-0 libglib2.0-dev libminizip-dev libwebp-dev libusb-1.0-0-dev libevdev-dev
```

#### Fedora
```bash
sudo dnf install -y git gcc-c++ cmake zlib-devel \
SDL3 SDL3_mixer SDL3_image SDL3_ttf \
SDL3-devel SDL3_mixer-devel SDL3_image-devel SDL3_ttf-devel \
gstreamer1 gstreamer1-plugins-base gstreamer1-plugins-good gstreamer1-libav \
glib2 glib2-devel minizip-devel libwebp-devel libusb1-devel libevdev-devel \
zlib libusb1 libevdev
```

#### Arch
```bash
sudo pacman -S git gcc cmake zlib \
sdl3 sdl3_mixer sdl3_image sdl3_ttf \
gstreamer gst-plugins-base gst-plugins-good gst-libav \
glib2 minizip libwebp libusb libevdev
```

### Download and compile the source code
Download the source code

	git clone https://github.com/CoinOPS-Official/RetroFE.git

Generate your gcc make files

	cd RetroFE
	cmake RetroFE/Source -BRetroFE/Build

Compile RetroFE

	cmake --build RetroFE/Build

The executable is then found in `/RetroFE/Build`

#   Building for MacOS #

## Install Homebrew

Both methods use Homebrew in some capacity (https://brew.sh)

## Universal2 Binaries

An Xcode project has been created to build universal binaries (x86_64 and arm64)

### Download the source code

	git clone https://github.com/CoinOPS-Official/RetroFE.git

### Install libraries

	curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.2.10/SDL3-3.2.10.dmg
	curl -LO https://github.com/libsdl-org/SDL_image/releases/download/release-3.2.4/SDL3_image-3.2.4.dmg
	curl -LO https://github.com/libsdl-org/SDL_mixer/releases/download/release-3.0.1/SDL3_mixer-3.0.1.dmg
	curl -LO https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.0/SDL3_ttf-3.2.0.dmg
	curl -LO https://gstreamer.freedesktop.org/data/pkg/osx/1.22.12/gstreamer-1.0-1.22.12-universal.pkg
	curl -LO https://gstreamer.freedesktop.org/data/pkg/osx/1.22.12/gstreamer-1.0-devel-1.22.12-universal.pkg
	
	sudo installer -pkg gstreamer-1.0-1.22.12-universal.pkg -target /
	sudo installer -pkg gstreamer-1.0-devel-1.22.12-universal.pkg -target /
	
	hdiutil attach SDL3-3.2.10.dmg
	cp -R /Volumes/SDL3/SDL3.framework RetroFE/RetroFE/ThirdPartyMac/
	hdiutil detach /Volumes/SDL3
	
	hdiutil attach SDL3_image-3.2.4.dmg
	cp -R /Volumes/SDL3_image/SDL3_image.framework RetroFE/RetroFE/ThirdPartyMac/
	cp -R /Volumes/SDL3_image/optional/webp.framework RetroFE/RetroFE/ThirdPartyMac/
	hdiutil detach /Volumes/SDL3_image
	
	hdiutil attach SDL3_mixer-3.0.1.dmg
	cp -R /Volumes/SDL3_mixer/SDL3_mixer.framework RetroFE/RetroFE/ThirdPartyMac/
	hdiutil detach /Volumes/SDL3_mixer
	
	hdiutil attach SDL3_ttf-3.2.0.dmg
	cp -R /Volumes/SDL3_ttf/SDL3_ttf.framework RetroFE/RetroFE/ThirdPartyMac/
	hdiutil detach /Volumes/SDL3_ttf
	
	cp -R /Library/Frameworks/GStreamer.framework RetroFE/RetroFE/ThirdPartyMac/
	
### Install headers

 ```bash
 brew install minizip libusb
 ```

### Compile the source code
Open the Xcodeproj in `RetroFE/xcode` and build target or

	cd RetroFE/
	xcodebuild -project RetroFE/xcode/retrofe.xcodeproj

The executable is then found in `/RetroFE/Build`

## Single Architecture Binaries
### Install libraries

```bash
brew install git gcc cmake zlib \
sdl2 sdl2_mixer sdl2_image sdl2_ttf \
gstreamer \
glib minizip webp libusb
```

### Download and compile the source code
Download the source code

	git clone https://github.com/CoinOPS-Official/RetroFE.git

Generate your gcc make files

	cd RetroFE
	cmake RetroFE/Source -BRetroFE/Build

Compile RetroFE

	cmake --build RetroFE/Build

The executable is then found in `/RetroFE/Build`

#   Optional #

###   Creating a test environment

A launchable test environment can be created with the following commands 

	python3 Scripts/Package.py --os=windows/linux/mac --build=full

Copy your live RetroFE system to any folder of your choosing. Files can be found in `Artifacts/{os}/RetroFE`

### Set $RETROFE_PATH via Environment variable 

RetroFE will load it's media and configuration files relative to where the binary file is located. This allows the build to be portable. If you want RetroFE to load your configuration from a fixed location regardless of where your install is copy your configuration there and set $RETROFE_PATH. Note this will work if you start RetroFE from the command line.

	vi ~/.bash_profile
	export RETROFE_PATH=/your/new/retrofe


### Set RETROFE_PATH via flat file 

Depending on your version of OS X the GUI will read user defined Environment variables from [another place](http://stackoverflow.com/questions/135688/setting-environment-variables-in-os-x). If you find this dificult to setup you can get around it by creating a text file in your HOME directory: /Users/<you>/.retrofe with one line no spaces: /your/new/retrofe. This will also work in Linux. RetroFE's configuration search order is 1st: ENV, Flat file, and executable location.

	echo /your/new/retrofe > ~/.retrofe

### Fix libpng iCCP warnings

The issue is with the png files that are being used with the Artwork. Libpng is pretty touchy about it. You can get rid of these messages with a handy tool called pngcrush found on sourceforge and github.

Error message:
	
	libpng warning: iCCP: known incorrect sRGB profile


Install pngcrush on Mac:    (linux use apt-get ?)
	
	brew install pngcrush


Use pngcrush to Find and repair pngs: 
	
	find /usr/local/opt/retrofe/collections -type f -iname '*.png' -exec pngcrush -ow -rem allb -reduce {} \;
