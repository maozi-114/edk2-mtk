#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BitmapLib.h>
#include <Library/KeypadDeviceImplLib.h>
#include <Library/UefiLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/KeypadDevice.h>

typedef enum {
  KEY_DEVICE_TYPE_UNKNOWN,
  KEY_DEVICE_TYPE_MATRIX,
  KEY_DEVICE_TYPE_PMIC
} KEY_DEVICE_TYPE;

typedef struct {
  KEY_CONTEXT     EfiKeyContext;
  BOOLEAN         IsValid;
  KEY_DEVICE_TYPE DeviceType;
  UINT32   MatrixIndex;
  UINT32   Button;
  BOOLEAN  PhysicalPressed;
} KEY_CONTEXT_PRIVATE;

#define WACS2_CMD       0x1000dc20
#define WACS2_RDATA     0x1000dc24
#define WACS2_VLDCLR    0x1000dc28
#define WACS2_INIT_DONE BIT22
#define WACS2_FSM_MASK  (7U << 16)
#define WACS2_WFVLDCLR  (6U << 16)
#define WACS2_POLL_MAX  1000

STATIC BOOLEAN PowerReadPending;
STATIC BOOLEAN PowerReadErrorReported;

// Serialize this library's WACS2 transaction against UEFI callbacks. A timed
// out read remains pending until its response can be discarded on a later poll.
STATIC BOOLEAN
ReadPowerButton (BOOLEAN *Pressed)
{
  EFI_TPL OldTpl;
  UINT32 Status;
  UINTN Poll;
  BOOLEAN Success = FALSE;

  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  for (Poll = 0; Poll < WACS2_POLL_MAX; Poll++) {
    Status = MmioRead32 (WACS2_RDATA);
    if ((Status & WACS2_INIT_DONE) == 0)
      goto Done;
    if (PowerReadPending && (Status & WACS2_FSM_MASK) == WACS2_WFVLDCLR) {
      MmioWrite32 (WACS2_VLDCLR, 1);
      PowerReadPending = FALSE;
      continue;
    }
    if (!PowerReadPending && (Status & WACS2_FSM_MASK) == 0)
      break;
  }
  if (Poll == WACS2_POLL_MAX)
    goto Done;

  MmioWrite32 (WACS2_CMD, 0x00120000); // PMIC register 0x24
  PowerReadPending = TRUE;
  for (Poll = 0; Poll < WACS2_POLL_MAX; Poll++) {
    Status = MmioRead32 (WACS2_RDATA);
    if ((Status & WACS2_INIT_DONE) == 0)
      goto Done;
    if ((Status & WACS2_FSM_MASK) == WACS2_WFVLDCLR) {
      *Pressed = (Status & BIT1) == 0;
      MmioWrite32 (WACS2_VLDCLR, 1);
      PowerReadPending = FALSE;
      Success = TRUE;
      break;
    }
  }
Done:
  gBS->RestoreTPL (OldTpl);
  if (!Success && !PowerReadErrorReported) {
    PowerReadErrorReported = TRUE;
    DEBUG ((DEBUG_ERROR, "[ROO:KEY] POWER read failed (WACS2 unavailable or timed out)\n"));
  }
  return Success;
}

UINTN gBitmapScanCodes[BITMAP_NUM_WORDS(0x18)]    = {0};
UINTN gBitmapUnicodeChars[BITMAP_NUM_WORDS(0x7f)] = {0};


EFI_KEY_DATA gKeyDataPowerDown = {.Key = {
                                      .UnicodeChar = (CHAR16)'s',
                                  }};

EFI_KEY_DATA gKeyDataPowerUp = {.Key = {
                                    .UnicodeChar = (CHAR16)'e',
                                }};

EFI_KEY_DATA gKeyDataPowerLongpress = {.Key = {
                                           .UnicodeChar = (CHAR16)' ',
                                       }};


#define MS2NS(ms) (((UINT64)(ms)) * 1000000ULL)

STATIC inline VOID
KeySetState(UINT16 ScanCode, CHAR16 UnicodeChar, BOOLEAN Value)
{
  if (ScanCode && ScanCode < 0x18) {
    if (Value)
      BitmapSet(gBitmapScanCodes, ScanCode);
    else
      BitmapClear(gBitmapScanCodes, ScanCode);
  }

  if (UnicodeChar && UnicodeChar < 0x7f) {
    if (Value)
      BitmapSet(gBitmapUnicodeChars, UnicodeChar);
    else
      BitmapClear(gBitmapUnicodeChars, UnicodeChar);
  }
}

STATIC inline BOOLEAN KeyGetState(UINT16 ScanCode, CHAR16 UnicodeChar)
{
  if (ScanCode && ScanCode < 0x18) {
    if (!BitmapTest(gBitmapScanCodes, ScanCode))
      return FALSE;
  }

  if (UnicodeChar && UnicodeChar < 0x7f) {
    if (!BitmapTest(gBitmapUnicodeChars, UnicodeChar))
      return FALSE;
  }

  return TRUE;
}

STATIC inline VOID LibKeyInitializeKeyContext(KEY_CONTEXT *Context)
{
  SetMem(&Context->KeyData, sizeof(Context->KeyData), 0);
  Context->Time      = 0;
  Context->State     = KEYSTATE_RELEASED;
  Context->Repeat    = FALSE;
  Context->Longpress = FALSE;
}

STATIC inline VOID LibKeyUpdateKeyStatus(
    KEY_CONTEXT *Context, KEYPAD_RETURN_API *KeypadReturnApi, BOOLEAN IsPressed,
    UINT64 Delta)
{
  // keep track of the actual state
  KeySetState(
      Context->KeyData.Key.ScanCode, Context->KeyData.Key.UnicodeChar,
      IsPressed);

  // update key time
  Context->Time += Delta;

  switch (Context->State) {
  case KEYSTATE_RELEASED:
    if (IsPressed) {
      // change to pressed
      Context->Time  = 0;
      Context->State = KEYSTATE_PRESSED;
    }
    break;

  case KEYSTATE_PRESSED:
    if (IsPressed) {
      // keyrepeat
      if (Context->Repeat && Context->Time >= MS2NS(200)) {
        KeypadReturnApi->PushEfikeyBufTail(KeypadReturnApi, &Context->KeyData);
        Context->Time   = 0;
        Context->Repeat = TRUE;
      }

      else if (!Context->Longpress && Context->Time >= MS2NS(500)) {
        // POWER, handle key combos
        if (Context->KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
          if (KeyGetState(SCAN_DOWN, 0)) {
            // report 's'
            KeypadReturnApi->PushEfikeyBufTail(
                KeypadReturnApi, &gKeyDataPowerDown);
          }
          else if (KeyGetState(SCAN_UP, 0)) {
            // report 'e'
            KeypadReturnApi->PushEfikeyBufTail(
                KeypadReturnApi, &gKeyDataPowerUp);
          }
          else {
            // report spacebar
            KeypadReturnApi->PushEfikeyBufTail(
                KeypadReturnApi, &gKeyDataPowerLongpress);
          }
        }

        // post first keyrepeat event
        else {
          // only start keyrepeat if we're not doing a combo
          if (!KeyGetState(0, CHAR_CARRIAGE_RETURN)) {
            KeypadReturnApi->PushEfikeyBufTail(
                KeypadReturnApi, &Context->KeyData);
            Context->Time   = 0;
            Context->Repeat = TRUE;
          }
        }

        Context->Longpress = TRUE;
      }
    }

    else {
      if (!Context->Longpress) {
        // we supressed down, so report it now
        KeypadReturnApi->PushEfikeyBufTail(KeypadReturnApi, &Context->KeyData);
        Context->State = KEYSTATE_LONGPRESS_RELEASE;
      }

      else if (Context->Time >= MS2NS(10)) {
        // we reported another key already
        Context->Time      = 0;
        Context->Repeat    = FALSE;
        Context->Longpress = FALSE;
        Context->State     = KEYSTATE_RELEASED;
      }
    }
    break;

  case KEYSTATE_LONGPRESS_RELEASE:
    // change to released
    Context->Time      = 0;
    Context->Repeat    = FALSE;
    Context->Longpress = FALSE;
    Context->State     = KEYSTATE_RELEASED;
    break;

  default:
    ASSERT(FALSE);
    break;
  }
}

STATIC KEY_CONTEXT_PRIVATE KeyContextVolumeDown;
STATIC KEY_CONTEXT_PRIVATE KeyContextVolumeUp;
STATIC KEY_CONTEXT_PRIVATE KeyContextPower;

STATIC KEY_CONTEXT_PRIVATE *KeyList[] = { &KeyContextVolumeDown, &KeyContextVolumeUp, &KeyContextPower };

STATIC
VOID KeypadInitializeKeyContextPrivate(KEY_CONTEXT_PRIVATE *Context)
{
  Context->IsValid     = FALSE;
  Context->PhysicalPressed = FALSE;
  Context->MatrixIndex = 0;
  Context->DeviceType  = KEY_DEVICE_TYPE_UNKNOWN;
}

STATIC
KEY_CONTEXT_PRIVATE *KeypadKeyCodeToKeyContext(UINT32 KeyCode)
{
  if (KeyCode == 114)
    return &KeyContextVolumeDown;
  else if (KeyCode == 115)
    return &KeyContextVolumeUp;
  else if (KeyCode == 116)
    return &KeyContextPower;
  else
    return NULL;
}

RETURN_STATUS
EFIAPI
KeypadDeviceImplConstructor(VOID)
{
  UINTN                Index;
  KEY_CONTEXT_PRIVATE *StaticContext;

  // Reset all keys
  for (Index = 0; Index < (sizeof(KeyList) / sizeof(KeyList[0])); Index++) {
    KeypadInitializeKeyContextPrivate(KeyList[Index]);
  }

  // Live FDT maps volume up/down to LK keypad matrix indices 0/1.
  StaticContext = KeypadKeyCodeToKeyContext(115);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_MATRIX;
  StaticContext->MatrixIndex = 0;
  StaticContext->IsValid = TRUE;
  StaticContext = KeypadKeyCodeToKeyContext(114);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_MATRIX;
  StaticContext->MatrixIndex = 1;
  StaticContext->IsValid = TRUE;

  // Power is read directly through the already-initialized WACS2 channel.
  StaticContext              = KeypadKeyCodeToKeyContext(116);
  StaticContext->DeviceType  = KEY_DEVICE_TYPE_PMIC;
  StaticContext->Button      = 1;
  StaticContext->IsValid     = TRUE;

  return RETURN_SUCCESS;
}

EFI_STATUS EFIAPI KeypadDeviceImplReset(KEYPAD_DEVICE_PROTOCOL *This)
{
  LibKeyInitializeKeyContext(&KeyContextVolumeDown.EfiKeyContext);
  KeyContextVolumeDown.EfiKeyContext.KeyData.Key.ScanCode = SCAN_DOWN;

  LibKeyInitializeKeyContext(&KeyContextVolumeUp.EfiKeyContext);
  KeyContextVolumeUp.EfiKeyContext.KeyData.Key.ScanCode = SCAN_UP;

  LibKeyInitializeKeyContext(&KeyContextPower.EfiKeyContext);
  KeyContextPower.EfiKeyContext.KeyData.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;

  return EFI_SUCCESS;
}

EFI_STATUS KeypadDeviceImplGetKeys(
    KEYPAD_DEVICE_PROTOCOL *This, KEYPAD_RETURN_API *KeypadReturnApi,
    UINT64 Delta)
{
  BOOLEAN IsPressed;
  UINTN Index;

  for (Index = 0; Index < (sizeof(KeyList) / sizeof(KeyList[0])); Index++) {
    KEY_CONTEXT_PRIVATE *Context = KeyList[Index];

    // check if this is a valid key
    if (Context->IsValid == FALSE)
      continue;

    IsPressed = FALSE;
    if (Context->DeviceType == KEY_DEVICE_TYPE_MATRIX) {
      UINT32 KeyIndex = Context->MatrixIndex;
      UINT16 Matrix = MmioRead16 (0x10010004 + 4 * (KeyIndex >> 4));
      IsPressed = (Matrix & (1U << (KeyIndex & 15))) == 0;
      if (IsPressed && !Context->PhysicalPressed) {
        DEBUG ((DEBUG_WARN, "[ROO:KEY] %a press\n", KeyIndex == 0 ? "UP" : "DOWN"));
      }
      Context->PhysicalPressed = IsPressed;
    } else if (Context->DeviceType == KEY_DEVICE_TYPE_PMIC) {
      if (!ReadPowerButton (&IsPressed))
        continue;
      if (IsPressed && !Context->PhysicalPressed)
        DEBUG ((DEBUG_WARN, "[ROO:KEY] POWER press\n"));
      Context->PhysicalPressed = IsPressed;
    } else {
      continue;
    }

    LibKeyUpdateKeyStatus(
        &Context->EfiKeyContext, KeypadReturnApi, IsPressed, Delta);
  }

  return EFI_SUCCESS;
}
