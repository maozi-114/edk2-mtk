[Defines]
  PLATFORM_NAME                  = roo
  PLATFORM_GUID                  = 54d47bf2-32f2-4532-9438-1b6971dc59c0
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = rooPkg/roo.fdf
  DEVICE_DXE_FV_COMPONENTS       = rooPkg/roo.fdf.inc

[PcdsFixedAtBuild.common]
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|921600
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetNxForStack|FALSE

!if $(TARGET) == DEBUG
  # Full standard DEBUG() stream on BaseDebugLibSerialPort/UART0.
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2F
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x807B55CF
  gEfiMdePkgTokenSpaceGuid.PcdFixedDebugPrintErrorLevel|0x807B55CF
!endif
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0xc0000000
  gArmTokenSpaceGuid.PcdCpuVectorBaseAddress|0x40C40000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackBase|0x40C00000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackSize|0x00040000
  gMediaTekTokenSpaceGuid.PcdUefiMemPoolBase|0x40C50000
  # Must end at 0x47C80000; the following 1 MiB is AEE/pstore/minidump.
  gMediaTekTokenSpaceGuid.PcdUefiMemPoolSize|0x07030000

  # Simple Framebuffer
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferWidth|480
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferHeight|640
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferAddress|0x7efe0000
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferLayers|0

[LibraryClasses]
  PlatformMemoryMapLib|rooPkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf

!include Silicon/MediaTek/MT6765Pkg/MT6765.dsc

[LibraryClasses.common]
  KeypadDeviceImplLib|rooPkg/Library/KeypadDeviceImplLib/KeypadDeviceImplLib.inf

# Keep RELEASE image placement while enabling the complete serial DEBUG() stream.
[PcdsFixedAtBuild.common]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2F
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x807B55CF
  gEfiMdePkgTokenSpaceGuid.PcdFixedDebugPrintErrorLevel|0x807B55CF

[PcdsFixedAtBuild.common]
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|3

[Components]
  rooPkg/Drivers/RooGic/ArmGicDxe.inf
  rooPkg/Drivers/RooGpio/RooGpio.inf
  rooPkg/Drivers/RooMsdcSnapshot/RooMsdcSnapshot.inf
