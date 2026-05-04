# SteamDeckHID – Windows Kernel Driver für den Steam Deck Controller (Neptune)

Dieses Projekt stellt zwei KMDF-Treiber bereit, um den integrierten Steam Deck Controller (Valve Neptune) unter Windows nativ nutzbar zu machen.

## Überblick

Der Stack besteht aus zwei Komponenten:

- **SteamDeckHID.sys**  
  HID-Eingangstreiber für das echte USB-Gerät. Leitet HID-Reports an den SteamDeckBus zur Emulation des Xbox360 Controller weiter

- **SteamDeckBus.sys**  
  Virtueller Bus mit emuliertem Xbox360-Controller. Der Bus-Treiber basiert auf einer stark reduzierten XUSB Emulation, ähnlich ViGEmBus.

## Features

- Native HID Gamepad-Unterstützung
- XInput-Kompatibilität (Xbox 360 Emulation)
- Kein zusätzlicher Userspace-Dienst notwendig
- Komplett im Kernel implementiert



## Architektur

```
Steam Deck Hardware
↓
usbccgp
↓
SteamDeckHID.sys
↓
DirectInput / SDL
↓
SteamDeckBus.sys
↓
Virtueller Xbox 360 Controller
↓
XInput
````

## Voraussetzungen

Für Build und Entwicklung:

- Windows 11
- Visual Studio 2022
- Windows Driver Kit (WDK)
- (Optional) WinDbg Preview für Debugging

> Wichtig: WDK und Windows SDK sollten dieselbe Version haben.


## How to Use

### 1. Test-Signing aktivieren

Treiber müssen signiert sein. Für lokale Tests:

```powershell
bcdedit /set testsigning on
````

→ Neustart 


### 2. Test-Zertifikat erstellen

```powershell
.\scripts\create-test-cert.ps1
```

### 3. Treiber bauen

```powershell
.\scripts\build.ps1 -Sign
```

Optionen:

```powershell
# Debug Build
.\scripts\build.ps1 -Configuration Debug -Sign

# Release Build
.\scripts\build.ps1 -Configuration Release -Sign

# Clean Build
.\scripts\build.ps1 -Configuration Release -Sign -Clean
```

### 4. Installation

```powershell
.\scripts\install.ps1
```

Optional:

```powershell
# Release installieren
.\scripts\install.ps1 -Configuration Release

# Deinstallieren
.\scripts\install.ps1 -Action uninstall
```

### 5. Funktion testen

* `joy.cpl` öffnen → Controller sollte sichtbar sein
* XInput-Spiel starten → sollte als Xbox Controller erkannt werden

## Status

Work in Progress.
Issues und Pull Requests sind willkommen.


## Referenzen

- [ViGEmBus](https://github.com/ViGEm/ViGEmBus) – Quellprojekt für `SteamDeckBus.sys` (BSD-3-Clause). Die XUSB-Descriptor-Bytes und die "Boot-Sequenz"-Blobs in [bus/BusPdo.cpp](bus/BusPdo.cpp) stammen unverändert daher;
- [Linux hid-steam.c](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-steam.c) – Valve's eigener Linux Kernel Treiber (Report-Strukturen)
- [neptune-hidapi.net](https://github.com/mKenfenheuer/neptune-hidapi.net) – C# HID-Library (Button-Mappings)
- [SWICD](https://github.com/mKenfenheuer/steam-deck-windows-usermode-driver) – Usermode Vorläufer dieses Projekts
- [KMDF HID Minidriver Sample](https://github.com/microsoft/Windows-driver-samples/tree/main/hid/hidusbfx2) – Microsoft Referenz-Implementierung
- [HID Usage Tables 1.4](https://usb.org/sites/default/files/hut1_4.pdf) – USB.org
- [WDK Dokumentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/) – Microsoft
