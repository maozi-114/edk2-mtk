#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

// Register layout and single-block PIO: MT6762 kernel ComboA sd.c/msdc_reg.h.
// This is an inherited, exclusively owned host; never reset or reinitialize it.
#define MSDC_BASE       0x11230000U
#define REG(Offset)     (MSDC_BASE + (Offset))
#define CMD_READY       (1U << 8)
#define CMD_ERRORS      ((1U << 9) | (1U << 10))
#define DATA_DONE       (1U << 12)
#define DATA_ERRORS     ((1U << 14) | (1U << 15))
#define CMD_INTERRUPTS  (CMD_READY | CMD_ERRORS)
#define DATA_INTERRUPTS (DATA_DONE | DATA_ERRORS)
#define R1_ERRORS       0xFFF9A080U
#define POLL_US         1000000U

STATIC
BOOLEAN
CardReady (UINT32 R1)
{
  return ((R1 & R1_ERRORS) == 0) && ((R1 & (1U << 8)) != 0) &&
         (((R1 >> 9) & 15U) == 4U);
}

STATIC
BOOLEAN
SendCommand (UINT32 Command, UINT32 Argument, UINT32 *Response)
{
  UINT32 Index, Pending;

  if ((MmioRead32 (REG (0x3c)) & 3U) != 0 || MmioRead32 (REG (0x0c)) != 0) {
    DEBUG ((DEBUG_WARN, "MSDC0 CMD%u refused: host busy or pending INT\n", Command & 63U));
    return FALSE;
  }
  MmioWrite32 (REG (0x38), Argument);
  MmioWrite32 (REG (0x34), Command);
  Pending = 0;
  for (Index = 0; Index < POLL_US; ++Index) {
    Pending = MmioRead32 (REG (0x0c)) & CMD_INTERRUPTS;
    if (Pending != 0) {
      break;
    }
    MicroSecondDelay (1);
  }
  if (Pending == 0) {
    DEBUG ((DEBUG_WARN, "MSDC0 CMD%u timeout INT=%08x STS=%08x\n", Command & 63U,
            MmioRead32 (REG (0x0c)), MmioRead32 (REG (0x3c))));
    return FALSE;
  }
  *Response = MmioRead32 (REG (0x40));
  MmioWrite32 (REG (0x0c), Pending);
  DEBUG ((DEBUG_WARN, "MSDC0 CMD%u INT=%08x R1=%08x\n", Command & 63U, Pending, *Response));
  return Pending == CMD_READY && ((*Response & R1_ERRORS) == 0);
}

STATIC
BOOLEAN
ReadBlock (UINT32 Opcode, UINT32 Argument, UINT8 *Buffer)
{
  UINT32 Index, Pending, Response, Count, Word, Bytes;
  BOOLEAN Done;

  if ((MmioRead32 (REG (0x14)) & 0xffffU) != 0) {
    DEBUG ((DEBUG_WARN, "MSDC0 single-block refused: FIFO not empty\n"));
    return FALSE;
  }
  // No reset/FIFO clear: set only PIO and the one-block transfer count.
  MmioOr32 (REG (0x00), 1U << 3);
  MmioWrite32 (REG (0x50), 1);
  DEBUG ((DEBUG_WARN, "MSDC0 single-block EXT_CSD PIO begin\n"));
  if (!SendCommand (Opcode | (1U << 7) | (1U << 11) | (512U << 16), Argument, &Response)) {
    return FALSE;
  }
  Done = FALSE;
  Bytes = 0;
  for (Index = 0; Index < POLL_US; ++Index) {
    Pending = MmioRead32 (REG (0x0c)) & DATA_INTERRUPTS;
    if (Pending != 0) {
      MmioWrite32 (REG (0x0c), Pending);
      if ((Pending & DATA_ERRORS) != 0) {
        DEBUG ((DEBUG_WARN, "MSDC0 single-block data error INT=%08x bytes=%u\n", Pending, Bytes));
        return FALSE;
      }
      Done = Done || ((Pending & DATA_DONE) != 0);
    }
    Count = MmioRead32 (REG (0x14)) & 0xffU;
    while (Count >= 4 && Bytes < 512) {
      Word = MmioRead32 (REG (0x1c));
      Buffer[Bytes++] = (UINT8)Word;
      Buffer[Bytes++] = (UINT8)(Word >> 8);
      Buffer[Bytes++] = (UINT8)(Word >> 16);
      Buffer[Bytes++] = (UINT8)(Word >> 24);
      Count -= 4;
    }
    if (Done && Bytes == 512) {
      DEBUG ((DEBUG_WARN, "MSDC0 single-block data complete bytes=%u\n", Bytes));
      return TRUE;
    }
    MicroSecondDelay (1);
  }
  DEBUG ((DEBUG_WARN, "MSDC0 single-block data timeout bytes=%u done=%u INT=%08x STS=%08x FIFO=%08x\n",
          Bytes, Done, MmioRead32 (REG (0x0c)), MmioRead32 (REG (0x3c)), MmioRead32 (REG (0x14))));
  return FALSE;
}

// Fixed diagnostic limits: no allocation and no unbounded media-controlled reads.
#define GPT_MAX_BYTES 65536U
STATIC UINT8 GptEntries[GPT_MAX_BYTES];
STATIC UINT8 GptHeader[512];

STATIC UINT32 Le32 (CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}
STATIC UINT64 Le64 (CONST UINT8 *P) {
  return (UINT64)Le32 (P) | ((UINT64)Le32 (P + 4) << 32);
}
STATIC BOOLEAN ReadSector (UINT32 Lba, UINT8 *Buffer) {
  UINT32 Response;
  return SendCommand (13U | (1U << 7), 1U << 16, &Response) && CardReady (Response) &&
         ReadBlock (17, Lba, Buffer);
}
STATIC VOID ReportGpt (UINT32 Sectors) {
  UINT32 HeaderSize, SavedCrc, Crc, Count, Size, Bytes, Blocks, I, J;
  UINT64 ArrayLba, FirstUsable, LastUsable, First, Last;
  UINT8 *Entry;
  CHAR16 Name[37];
  STATIC CONST UINT8 Signature[8] = { 'E','F','I',' ','P','A','R','T' };

  if (!ReadSector (1, GptHeader)) goto Invalid;
  for (I = 0; I < 8; ++I) if (GptHeader[I] != Signature[I]) goto Invalid;
  HeaderSize = Le32 (GptHeader + 12);
  if (Le32 (GptHeader + 8) != 0x00010000U || HeaderSize < 92 || HeaderSize > 512 ||
      Le32 (GptHeader + 20) != 0 || Le64 (GptHeader + 24) != 1 ||
      Le64 (GptHeader + 32) != (UINT64)Sectors - 1) goto Invalid;
  SavedCrc = Le32 (GptHeader + 16);
  for (I = 16; I < 20; ++I) GptHeader[I] = 0;
  if (EFI_ERROR (gBS->CalculateCrc32 (GptHeader, HeaderSize, &Crc)) || Crc != SavedCrc) goto Invalid;
  FirstUsable = Le64 (GptHeader + 40); LastUsable = Le64 (GptHeader + 48);
  ArrayLba = Le64 (GptHeader + 72); Count = Le32 (GptHeader + 80); Size = Le32 (GptHeader + 84);
  if (Count == 0 || Count > 128 || Size < 128 || Size > 512 || (Size & 7) != 0) goto Invalid;
  Bytes = Count * Size; Blocks = (Bytes + 511) / 512;
  if (FirstUsable < 2 || FirstUsable > LastUsable || LastUsable >= (UINT64)Sectors - 1 ||
      ArrayLba < 2 || ArrayLba >= Sectors || Blocks > Sectors - ArrayLba ||
      ArrayLba + Blocks > FirstUsable) goto Invalid;
  for (I = 0; I < Blocks; ++I)
    if (!ReadSector ((UINT32)ArrayLba + I, GptEntries + I * 512)) goto Invalid;
  if (EFI_ERROR (gBS->CalculateCrc32 (GptEntries, Bytes, &Crc)) || Crc != Le32 (GptHeader + 88)) goto Invalid;
  // Validate all entries before printing, so partial lists are never reported.
  for (I = 0; I < Count; ++I) {
    Entry = GptEntries + I * Size;
    for (J = 0; J < 16 && Entry[J] == 0; ++J) {}
    if (J == 16) continue;
    First = Le64 (Entry + 32); Last = Le64 (Entry + 40);
    if (First < FirstUsable || First > Last || Last > LastUsable) goto Invalid;
  }
  DEBUG ((DEBUG_WARN, "MSDC0 GPT validated entries=%u\n", Count));
  for (I = 0; I < Count; ++I) {
    Entry = GptEntries + I * Size;
    for (J = 0; J < 16 && Entry[J] == 0; ++J) {}
    if (J == 16) continue;
    for (J = 0; J < 36; ++J) Name[J] = (CHAR16)(Entry[56 + J * 2] | ((UINT16)Entry[57 + J * 2] << 8));
    Name[36] = 0;
    DEBUG ((DEBUG_WARN, "MSDC0 GPT %u name=%s first=%Lu last=%Lu\n", I + 1, Name,
            Le64 (Entry + 32), Le64 (Entry + 40)));
  }
  DEBUG ((DEBUG_WARN, "MSDC0 GPT end\n"));
  return;
Invalid:
  DEBUG ((DEBUG_WARN, "MSDC0 GPT unavailable or invalid; no partition list\n"));
}

EFI_STATUS
EFIAPI
RooMsdcSnapshotEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  UINT32 Cfg, Int, Inten, SdcSts, Response, Sectors;
  UINT8 ExtCsd[512];

  DEBUG ((DEBUG_WARN, "MSDC0 snapshot begin (inherited clock/access state unknown)\n"));
  Cfg = MmioRead32 (REG (0x00));
  Int = MmioRead32 (REG (0x0c));
  Inten = MmioRead32 (REG (0x10));
  SdcSts = MmioRead32 (REG (0x3c));
  DEBUG ((DEBUG_WARN, "MSDC0 CFG=%08x IOCON=%08x PS=%08x INT=%08x INTEN=%08x\n",
          Cfg, MmioRead32 (REG (0x04)), MmioRead32 (REG (0x08)), Int, Inten));
  DEBUG ((DEBUG_WARN, "MSDC0 FIFOCS=%08x SDC_CFG=%08x SDC_STS=%08x\n",
          MmioRead32 (REG (0x14)), MmioRead32 (REG (0x30)), SdcSts));
  if (((Cfg & ((1U << 0) | (1U << 7))) != ((1U << 0) | (1U << 7))) ||
      (Int != 0) || (Inten != 0) || ((SdcSts & 3U) != 0)) {
    DEBUG ((DEBUG_WARN, "MSDC0 probe skipped: host not idle/stable or interrupts pending/enabled\n"));
    return EFI_SUCCESS;
  }
  DEBUG ((DEBUG_WARN, "MSDC0 CMD13 RCA=1 begin\n"));
  if (!SendCommand ((1U << 7) | 13U, 1U << 16, &Response) || !CardReady (Response)) {
    DEBUG ((DEBUG_WARN, "MSDC0 CMD13 failed or card not ready in TRAN; no BlockIo/GPT\n"));
    return EFI_SUCCESS;
  }
  DEBUG ((DEBUG_WARN, "MSDC0 CMD13 RCA=1 state=%u ready=%u\n", (Response >> 9) & 15U, (Response >> 8) & 1U));
  if (!ReadBlock (8, 0, ExtCsd)) {
    DEBUG ((DEBUG_WARN, "MSDC0 EXT_CSD unavailable; no BlockIo/GPT\n"));
    return EFI_SUCCESS;
  }
  Sectors = (UINT32)ExtCsd[212] | ((UINT32)ExtCsd[213] << 8) |
            ((UINT32)ExtCsd[214] << 16) | ((UINT32)ExtCsd[215] << 24);
  DEBUG ((DEBUG_WARN, "MSDC0 EXT_CSD SEC_COUNT=%u PARTITION_CONFIG=%02x access=%u\n",
          Sectors, ExtCsd[179], ExtCsd[179] & 7U));
  if (!SendCommand ((1U << 7) | 13U, 1U << 16, &Response) || !CardReady (Response)) {
    DEBUG ((DEBUG_WARN, "MSDC0 post-single-block status invalid; no BlockIo/GPT\n"));
    return EFI_SUCCESS;
  }
  // Linux mmc_decode_ext_csd: revision >= 2 and density > 2 GiB imply
  // sector addressing (JEDEC); smaller cards are ambiguous without OCR.
  // LK observed OCR bit 30 set, consistent with this criterion. Require
  // 512-byte data sectors and leave the current partition access unchanged.
  if (ExtCsd[192] < 2 || ExtCsd[192] > 8 || Sectors <= 0x400000U ||
      (ExtCsd[179] & 7U) != 0 || ExtCsd[61] != 0) {
    DEBUG ((DEBUG_WARN, "MSDC0 ambiguous EXT_CSD rev=%u data_sector=%u; GPT withheld\n", ExtCsd[192], ExtCsd[61]));
    return EFI_SUCCESS;
  }
  ReportGpt (Sectors);
  return EFI_SUCCESS;
}
