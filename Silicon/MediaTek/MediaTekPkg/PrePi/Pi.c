// Pi.c: Entry point for SEC(Security).

#include <PiPei.h>
#include <Pi/PiBootMode.h>

#include <Library/PrePiLib.h>
#include <Library/PrintLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/PlatformPrePiLib.h>
#include <Library/SerialPortLib.h>

#include <Ppi/GuidedSectionExtraction.h>

#include "Pi.h"

#define IS_XIP()  (((UINT64)FixedPcdGet64 (PcdFdBaseAddress) > mSystemMemoryEnd) ||\
                  ((FixedPcdGet64 (PcdFdBaseAddress) + FixedPcdGet32 (PcdFdSize)) <= FixedPcdGet64 (PcdSystemMemoryBase)))

UINT64  mSystemMemoryEnd = FixedPcdGet64 (PcdSystemMemoryBase) +
                           FixedPcdGet64 (PcdSystemMemorySize) - 1;

VOID EFIAPI ProcessLibraryConstructorList(VOID);

volatile UINT32* wdt_reg = (volatile UINT32*)0x10007000;

VOID DisableWDT()
{
	*wdt_reg = 0x22000000;
}

VOID
PrePiMain(
  IN VOID *StackBase,
  IN UINTN StackSize
  )
{

  EFI_HOB_HANDOFF_INFO_TABLE *HobList;
  EFI_STATUS                  Status;

  UINTN MemoryBase     = 0;
  UINTN MemorySize     = 0;
  UINTN UefiMemoryBase = 0;
  UINTN UefiMemorySize = 0;

  // Architecture-specific initialization
  // Enable Floating Point
  ArmEnableVFP();

  if (ArmReadCurrentEL() == AARCH64_EL2) {
    // Trap General Exceptions. All exceptions that would be routed to EL1 are routed to EL2
    ArmWriteHcr(ARM_HCR_TGE);

    /* Enable Timer access for non-secure EL1 and EL0
       The cnthctl_el2 register bits are architecturally
       UNKNOWN on reset.
       Disable event stream as it is not in use at this stage
    */
    ArmWriteCntHctl(CNTHCTL_EL2_EL1PCTEN | CNTHCTL_EL2_EL1PCEN);
  }

  /* Enable program flow prediction, if supported */
  ArmEnableBranchPrediction();

  // Declare UEFI region
  MemoryBase     = FixedPcdGet32(PcdSystemMemoryBase);
  MemorySize     = FixedPcdGet32(PcdSystemMemorySize);
  UefiMemoryBase = FixedPcdGet32(PcdUefiMemPoolBase);
  UefiMemorySize = FixedPcdGet32(PcdUefiMemPoolSize);
  StackBase      = (VOID *)(UefiMemoryBase + UefiMemorySize - StackSize);

  DEBUG(
      (EFI_D_INFO | EFI_D_LOAD,
       "UEFI Memory Base = 0x%llx, Size = 0x%llx \n"
       "Stack Base = 0x%llx, Stack Size = 0x%llx \n",
       UefiMemoryBase, UefiMemorySize, StackBase, StackSize));

  // Set up HOB
  HobList = HobConstructor(
      (VOID *)UefiMemoryBase, UefiMemorySize, (VOID *)UefiMemoryBase,
      StackBase);

  PrePeiSetHobList (HobList);

  // Invalidate cache
  InvalidateDataCacheRange(
      (VOID *)(UINTN)PcdGet64(PcdFdBaseAddress), PcdGet32(PcdFdSize));

  SerialPortWrite ((UINT8 *) "[ROO:M1]", 8);
  Status = MemoryPeim(UefiMemoryBase, UefiMemorySize);
  SerialPortWrite ((UINT8 *) "[ROO:M2]", 8);
  ASSERT_EFI_ERROR (Status);

  // Add HOBs
  BuildStackHob ((UINTN)StackBase, StackSize);

  // TODO: Call CpuPei as a library
  BuildCpuHob (ArmGetPhysicalAddressBits (), PcdGet8 (PcdPrePiCpuIoSize));

  // Set the Boot Mode
  SetBootMode (BOOT_WITH_DEFAULT_SETTINGS);

  // Initialize Platform HOBs (CpuHob and FvHob)
  DEBUG((EFI_D_INFO, "PlatformPeim In \n"));
  SerialPortWrite ((UINT8 *) "[ROO:P1]", 8);
  Status = PlatformPeim();
  SerialPortWrite ((UINT8 *) "[ROO:P2]", 8);
  ASSERT_EFI_ERROR (Status);

  // Now, the HOB List has been initialized, we can register performance information
  // PERF_START (NULL, "PEI", NULL, StartTimeStamp);

  // SEC phase needs to run library constructors by hand.
  ProcessLibraryConstructorList();

  // Assume the FV that contains the SEC (our code) also contains a compressed FV.
  DEBUG((EFI_D_INFO, "DecompressFirstFv In \n"));
  SerialPortWrite ((UINT8 *) "[ROO:D1]", 8);
  Status = DecompressFirstFv();
  SerialPortWrite ((UINT8 *) "[ROO:D2]", 8);
  ASSERT_EFI_ERROR (Status);

  // Load the DXE Core and transfer control to it
  DEBUG((EFI_D_INFO, "LoadDxeCoreFromFv In \n"));
  SerialPortWrite ((UINT8 *) "[ROO:L1]", 8);
  Status = LoadDxeCoreFromFv(NULL, 0);
  SerialPortWrite ((UINT8 *) "[ROO:L2]", 8);
  ASSERT_EFI_ERROR (Status);
}

VOID SerialPortInit (VOID)
{
  SerialPortInitialize ();

  DEBUG ((EFI_D_INFO, "\nEDK II For MediaTek Project (AArch64)\n"));
  DEBUG (
      (EFI_D_INFO, "Firmware version %s built %a %a\n\n",
       (CHAR16 *)PcdGetPtr(PcdFirmwareVersionString), __TIME__, __DATE__));
}

VOID
CEntryPoint(
  IN VOID *StackBase,
  IN UINTN StackSize
  )
{
  SerialPortInit ();
  SerialPortWrite ((UINT8 *) "[ROO] PREPI: CEntryPoint\r\n", sizeof ("[ROO] PREPI: CEntryPoint\r\n") - 1);

  // Do platform specific initialization here
  PlatformInitialize();
  SerialPortWrite ((UINT8 *) "[ROO] PREPI: PlatformInitialize done\r\n", sizeof ("[ROO] PREPI: PlatformInitialize done\r\n") - 1);

  // Goto primary Main.
  SerialPortWrite ((UINT8 *) "[ROO] PREPI: entering PrePiMain\r\n", sizeof ("[ROO] PREPI: entering PrePiMain\r\n") - 1);
  PrePiMain(StackBase, StackSize);

  // DXE Core should always load and never return
  ASSERT(FALSE);
}

VOID
SecondaryCEntryPoint(
  IN  UINTN  MpId
  )
{
  // We must never get into this function on UniCore system
  ASSERT(FALSE);
}
