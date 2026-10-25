/** @file
  MediaTek MT676x UART SerialPortLib implementation.

  The register layout and baud-rate algorithm follow the MT6762/MT6739
  Linux UART driver.  This platform uses UART0 at 0x11002000 with a
  26 MHz source clock.
**/

#include <Base.h>
#include <Library/SerialPortLib.h>
#include <Library/PcdLib.h>
#include <Protocol/SerialIo.h>

#define MTK_UART_BASE          0x11002000U
#define MTK_UART_CLOCK         26000000U

#define UART_RBR_THR           0x00
#define UART_IER_DLH           0x04
#define UART_FCR               0x08
#define UART_LCR               0x0C
#define UART_MCR               0x10
#define UART_LSR               0x14
#define UART_MSR               0x18
#define UART_HIGHSPEED         0x24
#define UART_SAMPLE_COUNT      0x28
#define UART_SAMPLE_POINT      0x2C
#define UART_RATE_FIX_AD       0x34
#define UART_GUARD             0x3C

#define UART_LCR_WLS_8         0x03
#define UART_LCR_STOP_2        0x04
#define UART_LCR_PARITY_EN     0x08
#define UART_LCR_EVEN_PARITY   0x10
#define UART_LCR_STICK_PARITY  0x20
#define UART_LCR_DLAB          0x80

#define UART_FCR_ENABLE        0x01
#define UART_FCR_CLEAR_RX      0x02
#define UART_FCR_CLEAR_TX      0x04
#define UART_FCR_NORMAL        (UART_FCR_ENABLE | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX | (1U << 4) | (2U << 6))

#define UART_LSR_DR            0x01
#define UART_LSR_THRE          0x20
#define UART_LSR_TEMT          0x40

#define UART_MCR_DTR           0x01
#define UART_MCR_RTS           0x02
#define UART_MSR_CTS           0x10
#define UART_MSR_DSR           0x20
#define UART_MSR_RI            0x40
#define UART_MSR_DCD           0x80

STATIC
UINT32
UartRead (
  IN UINTN  Offset
  )
{
  return *(volatile UINT32 *)(UINTN)(MTK_UART_BASE + Offset);
}

STATIC
VOID
UartWrite (
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  *(volatile UINT32 *)(UINTN)(MTK_UART_BASE + Offset) = Value;
}

STATIC
UINT32
UartLineControl (
  IN UINT8              DataBits,
  IN EFI_PARITY_TYPE    Parity,
  IN EFI_STOP_BITS_TYPE StopBits
  )
{
  UINT32  Lcr;

  switch (DataBits) {
    case 5: Lcr = 0; break;
    case 6: Lcr = 1; break;
    case 7: Lcr = 2; break;
    case 8: Lcr = UART_LCR_WLS_8; break;
    default: return MAX_UINT32;
  }

  if (StopBits == TwoStopBits || ((StopBits == OneFiveStopBits) && (DataBits == 5))) {
    Lcr |= UART_LCR_STOP_2;
  } else if (StopBits != OneStopBit && StopBits != DefaultStopBits) {
    return MAX_UINT32;
  }

  switch (Parity) {
    case DefaultParity:
    case NoParity:
      break;
    case OddParity:
      Lcr |= UART_LCR_PARITY_EN;
      break;
    case EvenParity:
      Lcr |= UART_LCR_PARITY_EN | UART_LCR_EVEN_PARITY;
      break;
    case MarkParity:
      Lcr |= UART_LCR_PARITY_EN | UART_LCR_STICK_PARITY;
      break;
    case SpaceParity:
      Lcr |= UART_LCR_PARITY_EN | UART_LCR_EVEN_PARITY | UART_LCR_STICK_PARITY;
      break;
    default:
      return MAX_UINT32;
  }

  return Lcr;
}

STATIC
RETURN_STATUS
UartConfigure (
  IN UINT64             BaudRate,
  IN UINT8              DataBits,
  IN EFI_PARITY_TYPE    Parity,
  IN EFI_STOP_BITS_TYPE StopBits
  )
{
  UINT32  Lcr;
  UINT32  Divisor;
  UINT32  Remainder;
  UINT32  HighSpeed;
  UINT32  HighDiv;
  UINT32  SampleCount;
  UINT32  SamplePoint;

  if ((BaudRate == 0) || (BaudRate > (MTK_UART_CLOCK / 4))) {
    return RETURN_UNSUPPORTED;
  }

  Lcr = UartLineControl (DataBits, Parity, StopBits);
  if (Lcr == MAX_UINT32) {
    return RETURN_UNSUPPORTED;
  }

  UartWrite (UART_RATE_FIX_AD, 0);
  UartWrite (UART_LCR, Lcr);
  UartWrite (UART_FCR, UART_FCR_NORMAL);

  if (BaudRate <= 115200) {
    HighSpeed = 0;
    Divisor  = (MTK_UART_CLOCK >> 4) / (UINT32)BaudRate;
    Remainder = (MTK_UART_CLOCK >> 4) % (UINT32)BaudRate;
    if (Remainder >= ((UINT32)BaudRate / 2)) {
      Divisor++;
    }
    UartWrite (UART_HIGHSPEED, HighSpeed);
    UartWrite (UART_LCR, Lcr | UART_LCR_DLAB);
    UartWrite (UART_RBR_THR, Divisor & 0xFF);
    UartWrite (UART_IER_DLH, (Divisor >> 8) & 0xFF);
    UartWrite (UART_LCR, Lcr);
  } else if (BaudRate <= 460800) {
    HighSpeed = 2;
    Divisor  = (MTK_UART_CLOCK >> 2) / (UINT32)BaudRate;
    Remainder = (MTK_UART_CLOCK >> 2) % (UINT32)BaudRate;
    if (Remainder >= ((UINT32)BaudRate / 2)) {
      Divisor++;
    }
    UartWrite (UART_HIGHSPEED, HighSpeed);
    UartWrite (UART_LCR, Lcr | UART_LCR_DLAB);
    UartWrite (UART_RBR_THR, Divisor & 0xFF);
    UartWrite (UART_IER_DLH, (Divisor >> 8) & 0xFF);
    UartWrite (UART_LCR, Lcr);
  } else {
    HighSpeed = 3;
    HighDiv   = ((MTK_UART_CLOCK / (UINT32)BaudRate) >> 8) + 1;
    Divisor   = MTK_UART_CLOCK / ((UINT32)BaudRate * HighDiv);
    if (Divisor == 0) {
      return RETURN_UNSUPPORTED;
    }
    SampleCount = Divisor - 1;
    SamplePoint = (SampleCount == 0) ? 0 : ((SampleCount - 1) >> 1);
    UartWrite (UART_HIGHSPEED, HighSpeed);
    UartWrite (UART_LCR, Lcr | UART_LCR_DLAB);
    UartWrite (UART_RBR_THR, HighDiv & 0xFF);
    UartWrite (UART_IER_DLH, (HighDiv >> 8) & 0xFF);
    UartWrite (UART_LCR, Lcr);
    UartWrite (UART_SAMPLE_COUNT, SampleCount);
    UartWrite (UART_SAMPLE_POINT, SamplePoint);
    if (BaudRate >= 3000000) {
      UartWrite (UART_GUARD, 0x12);
    }
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  return UartConfigure (FixedPcdGet64 (PcdUartDefaultBaudRate), 8, NoParity, OneStopBit);
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  if (Buffer == NULL) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    if (Buffer[Index] == '\n') {
      while ((UartRead (UART_LSR) & UART_LSR_THRE) == 0) {}
      UartWrite (UART_RBR_THR, '\r');
    }
    while ((UartRead (UART_LSR) & UART_LSR_THRE) == 0) {}
    UartWrite (UART_RBR_THR, Buffer[Index]);
  }

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  UINTN  Count;

  if (Buffer == NULL) {
    return 0;
  }

  for (Count = 0; (Count < NumberOfBytes) && SerialPortPoll (); Count++) {
    Buffer[Count] = (UINT8)UartRead (UART_RBR_THR);
  }

  return Count;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return (BOOLEAN)((UartRead (UART_LSR) & UART_LSR_DR) != 0);
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  UINT32  Mcr;

  if ((Control & ~(EFI_SERIAL_REQUEST_TO_SEND | EFI_SERIAL_DATA_TERMINAL_READY)) != 0) {
    return RETURN_UNSUPPORTED;
  }

  Mcr = UartRead (UART_MCR);
  Mcr &= ~(UART_MCR_DTR | UART_MCR_RTS);
  if ((Control & EFI_SERIAL_DATA_TERMINAL_READY) != 0) {
    Mcr |= UART_MCR_DTR;
  }
  if ((Control & EFI_SERIAL_REQUEST_TO_SEND) != 0) {
    Mcr |= UART_MCR_RTS;
  }
  UartWrite (UART_MCR, Mcr);

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  UINT32  Lsr;
  UINT32  Mcr;
  UINT32  Msr;

  if (Control == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  *Control = 0;
  Msr = UartRead (UART_MSR);
  Mcr = UartRead (UART_MCR);
  Lsr = UartRead (UART_LSR);

  if ((Msr & UART_MSR_CTS) != 0) { *Control |= EFI_SERIAL_CLEAR_TO_SEND; }
  if ((Msr & UART_MSR_DSR) != 0) { *Control |= EFI_SERIAL_DATA_SET_READY; }
  if ((Msr & UART_MSR_RI)  != 0) { *Control |= EFI_SERIAL_RING_INDICATE; }
  if ((Msr & UART_MSR_DCD) != 0) { *Control |= EFI_SERIAL_CARRIER_DETECT; }
  if ((Mcr & UART_MCR_DTR) != 0) { *Control |= EFI_SERIAL_DATA_TERMINAL_READY; }
  if ((Mcr & UART_MCR_RTS) != 0) { *Control |= EFI_SERIAL_REQUEST_TO_SEND; }
  if ((Lsr & UART_LSR_DR) == 0) { *Control |= EFI_SERIAL_INPUT_BUFFER_EMPTY; }
  if ((Lsr & (UART_LSR_THRE | UART_LSR_TEMT)) == (UART_LSR_THRE | UART_LSR_TEMT)) {
    *Control |= EFI_SERIAL_OUTPUT_BUFFER_EMPTY;
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64             *BaudRate,
  IN OUT UINT32             *ReceiveFifoDepth,
  IN OUT UINT32             *Timeout,
  IN OUT EFI_PARITY_TYPE    *Parity,
  IN OUT UINT8              *DataBits,
  IN OUT EFI_STOP_BITS_TYPE *StopBits
  )
{
  RETURN_STATUS  Status;

  if ((BaudRate == NULL) || (ReceiveFifoDepth == NULL) || (Timeout == NULL) ||
      (Parity == NULL) || (DataBits == NULL) || (StopBits == NULL)) {
    return RETURN_INVALID_PARAMETER;
  }

  if (*BaudRate == 0) {
    *BaudRate = FixedPcdGet64 (PcdUartDefaultBaudRate);
  }
  if (*ReceiveFifoDepth == 0) {
    *ReceiveFifoDepth = 1;
  }
  if (*Parity == DefaultParity) {
    *Parity = NoParity;
  }
  if (*DataBits == 0) {
    *DataBits = 8;
  }
  if (*StopBits == DefaultStopBits) {
    *StopBits = OneStopBit;
  }

  Status = UartConfigure (*BaudRate, *DataBits, *Parity, *StopBits);
  return Status;
}
