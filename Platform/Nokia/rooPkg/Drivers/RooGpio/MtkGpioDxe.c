#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MtkGpioImplLib.h>
#include <Library/DebugLib.h>

#include <Protocol/MtkGpio.h>

/* Offset for Set|Get values */
#define PIN_SG_OFFSET(P)   ((P / 32) * 0x10)
#define PIN_MODE_OFFSET(P) ((P / 8) * 0x10)
#define PIN_MODE_BIT(P)    (((P) % 8) * 4)

VOID
GpioWrite (
  IN UINT32    Reg,
  IN UINT32    Value
  )
{
  MmioWrite32(PlatformInfo.BaseAddr + Reg, Value);
}

VOID
GpioRead (
  IN  UINT32    Reg,
  OUT UINT32   *Value
  )
{
  *Value = MmioRead32(PlatformInfo.BaseAddr + Reg);
}

// MaxPin is a count (MT6765 exposes pins 0 through 178).
#define VALID_PIN(P) ((P) < PlatformInfo.MaxPin)

VOID
GpioSetMode (UINT32 Pin, MTK_GPIO_MODE Mode)
{
  UINT32 Offset, Bit, ModeNum;
  if (!VALID_PIN (Pin)) return;
  Offset = PlatformInfo.ModeOffset + PIN_MODE_OFFSET (Pin);
  Bit = PIN_MODE_BIT (Pin);
  ModeNum = (UINT32) Mode & 7U;
  GpioWrite (Offset + PlatformInfo.ResetOffset, 7U << Bit);
  GpioWrite (Offset + PlatformInfo.SetOffset, ModeNum << Bit);
}

VOID
GpioSetDir (UINT32 Pin, BOOLEAN Direction)
{
  UINT32 Offset;
  if (!VALID_PIN (Pin)) return;
  Offset = PlatformInfo.DirOffset + PIN_SG_OFFSET (Pin);
  Offset += Direction ? PlatformInfo.ResetOffset : PlatformInfo.SetOffset;
  GpioWrite (Offset, 1U << (Pin % 32));
}

VOID
GpioGetDir (UINT32 Pin, BOOLEAN *Direction)
{
  UINT32 Reg;
  if (!VALID_PIN (Pin) || Direction == NULL) return;
  GpioRead (PlatformInfo.DirOffset + PIN_SG_OFFSET (Pin), &Reg);
  *Direction = (Reg & (1U << (Pin % 32))) == 0;
}

VOID
GpioGetOut (UINT32 Pin, BOOLEAN *Status)
{
  UINT32 Reg;
  if (!VALID_PIN (Pin) || Status == NULL) return;
  GpioRead (PlatformInfo.DataOutOffset + PIN_SG_OFFSET (Pin), &Reg);
  *Status = (Reg & (1U << (Pin % 32))) != 0;
}

VOID
GpioGetIn (UINT32 Pin, BOOLEAN *Status)
{
  UINT32 Reg;
  if (!VALID_PIN (Pin) || Status == NULL) return;
  GpioRead (PlatformInfo.DataInOffset + PIN_SG_OFFSET (Pin), &Reg);
  *Status = (Reg & (1U << (Pin % 32))) != 0;
}

VOID
GpioSet (UINT32 Pin, BOOLEAN Status)
{
  UINT32 Offset;
  if (!VALID_PIN (Pin)) return;
  Offset = PlatformInfo.DataOutOffset + PIN_SG_OFFSET (Pin);
  Offset += Status ? PlatformInfo.SetOffset : PlatformInfo.ResetOffset;
  GpioWrite (Offset, 1U << (Pin % 32));
}

VOID
GpioGet (UINT32 Pin, BOOLEAN *Status)
{
  BOOLEAN Direction;
  if (!VALID_PIN (Pin) || Status == NULL) return;
  GpioGetDir (Pin, &Direction);
  if (Direction) GpioGetIn (Pin, Status);
  else GpioGetOut (Pin, Status);
}

// MT6765 supplies no drive or pull mappings; do not access unmapped registers.
VOID GpioSetDrv (UINT32 Pin, UINT32 Drv) { (VOID)Pin; (VOID)Drv; }
VOID GpioSetR0 (UINT32 Pin) { (VOID)Pin; }
VOID GpioSetR1 (UINT32 Pin) { (VOID)Pin; }
VOID GpioSetPupd (UINT32 Pin, BOOLEAN IsPullDown) { (VOID)Pin; (VOID)IsPullDown; }

MTK_GPIO MtkGpio = {
  GpioGet,
  GpioSet,
  GpioGetDir,
  GpioSetDir,
  GpioSetMode,
  GpioSetDrv,
  GpioSetR0,
  GpioSetR1,
  GpioSetPupd
};

EFI_STATUS
EFIAPI
MtkGpioInitialize(
  IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  Status = gBS->InstallMultipleProtocolInterfaces(
    &ImageHandle,
    &gMtkGpioProtocolGuid,
    &MtkGpio,
    NULL);

  return Status;
}
