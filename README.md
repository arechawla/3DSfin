# 3DSFin
3DSfin is a *work in progress* jellyfin client for New Nintendo 3DS.


## Install 
 ### Prerequisites
  - [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group installed

  ### Build

  **Linux / macOS**
  ```bash
  export DEVKITPRO=/opt/devkitpro
  export DEVKITARM=/opt/devkitpro/devkitARM
  export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
  make
```

  **Windows (PowerShell)**
  ```
  $env:DEVKITPRO = "C:/devkitPro"
  $env:DEVKITARM = "C:/devkitPro/devkitARM"
  $env:PATH = "C:\devkitPro\msys2\usr\bin;C:\devkitPro\devkitARM\bin;C:\devkitPro\tools\bin;$env:PATH"
  C:\devkitPro\msys2\usr\bin\make.exe

  Output: 3dsfin.3dsx
  ```

## Setup
After moving `3dsfin.3dsx` to 3DS, launch homebrew to find application:
![Screenshot](3dsfinhomebrewlauncher.jpg)
Login with server IP and user credentials:
![Screenshot](3dsfinlogindualscreen.jpg)
Connection will start:
![Screenshot](connecting.jpg)
Full Library Catalog can be browsed:
![Screenshot](mainscreen.jpg)



## Current Status
3DSfin is in very early development, with full video playback **not supported** as of yet. As of the latest release, 3DSfin suports the following features:
- Server Adding: Enter your jellyfin server URL via the on-screen keyboard
- Authentication: Login with your Jellyfin username and password
- Full Library Browsing: Scrollable list of all your Jellyfin Libraries. Browse
- Metadata Viwe: Displays title, year, duration, and media type
- Stream URL retrieval: Requests a server-transcoded H.264/AAC MPEG-TS stream at 400×240 (240p) and 1.5 Mbps
- Persistent config — Server URL and username are saved to /3ds/3dsfin/config.ini on the SD card between sessions


