# 3DSFin
**Video and audio playback of your entire jellyfin catalog, on your new 3ds!**

3DSfin is a *work in progress* jellyfin client for New Nintendo 3DS. 


## Install 

 ### 1. Remote Install (FBI):
 ![QR](3dsfinqr.png)

 ### 2. Manual Install
 Donwload from releases tab and drag to `cias` folder on sd card. 

 ### 3. Build From Source:

 #### Prerequisites
  - [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group installed
  - [MakeRom](https://github.com/3DSGuy/Project_CTR/releases/tag/makerom-v0.18.3) installed in `~\devkitPro\tools\bin` for .cia build

  #### Build

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
  ```

Output: `3dsfin.3dsx, 3dsfin.cia`


## Setup
Login with server IP and user credentials:
![Screenshot](3dsfinlogindualscreen.jpg)
Connection will start:
![Screenshot](connecting.jpg)
Full Library Catalog can be browsed:
![Screenshot](mainscreennew.jpg)
Browsing Within Library:
![Screenshot](librarybrowse.jpg)
Playback:
![Screenshot](playback.jpg)



## Current Status
3DSfin is in very early (alpha) development. Check out release notes for versions in releases tab for the latest updates.


## Credits
- thanks to [FourthTube](https://github.com/erievs/FourthTube), for figuring out streaming logic and audio sync issues
