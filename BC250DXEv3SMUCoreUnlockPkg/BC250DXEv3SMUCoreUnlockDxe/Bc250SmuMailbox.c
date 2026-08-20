#include "Bc250SmuMailbox.h"

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PciSegmentLib.h>
#include <Library/TimerLib.h>

//
// Host bridge PCI address for the SMN indirect access window.
//
#define BC250_HOST_PCI_SEGMENT  0
#define BC250_HOST_PCI_BUS      0
#define BC250_HOST_PCI_DEVICE   0
#define BC250_HOST_PCI_FUNCTION 0

#define BC250_PCI_SEGMENT_ADDRESS(Offset) \
  PCI_SEGMENT_LIB_ADDRESS (BC250_HOST_PCI_SEGMENT, BC250_HOST_PCI_BUS, BC250_HOST_PCI_DEVICE, \
                           BC250_HOST_PCI_FUNCTION, (Offset))

//
// Number of Queue 3 argument DWORDs.
//
#define BC250_Q3_ARG_COUNT  6

/**
  Wait for the Queue 3 response register to reach a terminal state or for the
  timeout to expire.

  @param[out] Status     Last value observed in Q3_RSP.
  @param[in]  TimeoutUs  Maximum time to wait, in microseconds.

  @retval EFI_SUCCESS  A terminal Queue 3 status was observed.
  @retval EFI_TIMEOUT  The queue never reached a terminal state in time.
**/
STATIC
EFI_STATUS
WaitForDoneStatus (
  OUT UINT32  *Status,
  IN  UINT64  TimeoutUs
  )
{
  UINT64  Start;
  UINT64  ElapsedUs;
  UINT32  Value;

  Start = GetPerformanceCounter ();

  for (;;) {
    Value = Bc250SmnRead32 (BC250_Q3_RSP);
    if (Bc250SmuIsDoneStatus (Value)) {
      *Status = Value;
      return EFI_SUCCESS;
    }

    ElapsedUs = DivU64x32 (GetTimeInNanoSecond (GetPerformanceCounter () - Start), 1000U);
    if (ElapsedUs >= TimeoutUs) {
      *Status = Value;
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (BC250_MAILBOX_POLL_DELAY_US);
  }
}

UINT32
Bc250SmnRead32 (
  IN UINT32  Register
  )
{
  PciSegmentWrite32 (BC250_PCI_SEGMENT_ADDRESS (BC250_SMN_INDEX_OFFSET), Register);
  return PciSegmentRead32 (BC250_PCI_SEGMENT_ADDRESS (BC250_SMN_DATA_OFFSET));
}

VOID
Bc250SmnWrite32 (
  IN UINT32  Register,
  IN UINT32  Value
  )
{
  PciSegmentWrite32 (BC250_PCI_SEGMENT_ADDRESS (BC250_SMN_INDEX_OFFSET), Register);
  PciSegmentWrite32 (BC250_PCI_SEGMENT_ADDRESS (BC250_SMN_DATA_OFFSET), Value);
}

BOOLEAN
Bc250SmuIsDoneStatus (
  IN UINT32  Status
  )
{
  return (BOOLEAN)(Status == BC250_SMU_RETURN_OK ||
                    Status == BC250_SMU_RETURN_FAILED ||
                    Status == BC250_SMU_RETURN_UNKNOWN_CMD ||
                    Status == BC250_SMU_RETURN_REJECTED_PREREQ ||
                    Status == BC250_SMU_RETURN_REJECTED_BUSY);
}

EFI_STATUS
Bc250SmuSendQueue3Message (
  IN  UINT32  Command,
  IN  UINTN   Argc,
  IN  UINT32  *Args,
  OUT UINT32  *Status
  )
{
  EFI_STATUS  Result;
  UINTN       Index;
  UINT32      ArgValue;

  *Status = 0;

  Result = WaitForDoneStatus (Status, BC250_MAILBOX_TIMEOUT_US);
  if (EFI_ERROR (Result)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: SMU mailbox not idle, last status=0x%08x\n", *Status));
    return Result;
  }

  Bc250SmnWrite32 (BC250_Q3_RSP, 0);

  for (Index = 0; Index < BC250_Q3_ARG_COUNT; Index++) {
    if (Args != NULL && Index < Argc) {
      ArgValue = Args[Index];
    } else {
      ArgValue = 0;
    }
    Bc250SmnWrite32 (BC250_Q3_ARG + (UINT32)(Index * sizeof (UINT32)), ArgValue);
  }

  Bc250SmnWrite32 (BC250_Q3_CMD, Command);

  Result = WaitForDoneStatus (Status, BC250_MAILBOX_TIMEOUT_US);
  if (EFI_ERROR (Result)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: SMU mailbox timed out after command 0x%08x\n", Command));
  }

  return Result;
}

BOOLEAN
Bc250SmuSecureAccessEnabled (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Rsp;

  Status = Bc250SmuSendQueue3Message (BC250_Q3_MSG_SMN_READ32, 0, NULL, &Rsp);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  return (BOOLEAN)(Rsp != BC250_SMU_RETURN_REJECTED_PREREQ);
}

EFI_STATUS
Bc250SmuWriteSmn (
  IN UINT32  Address,
  IN UINT32  Value
  )
{
  EFI_STATUS  Status;
  UINT32      Rsp;
  UINT32      SetAddr;

  if (!Bc250SmuSecureAccessEnabled ()) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: protected SMU functions unavailable, cannot write mask\n"));
    return EFI_DEVICE_ERROR;
  }

  SetAddr = Address;
  Status = Bc250SmuSendQueue3Message (BC250_Q3_MSG_SET_SMN_WRITE_ADDR, 1, &SetAddr, &Rsp);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: SMU set write addr rejected: %r (rsp=0x%08x)\n", Status, Rsp));
    return Status;
  }
  if (Rsp != BC250_SMU_RETURN_OK) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: SMU set write addr returned 0x%08x\n", Rsp));
    return EFI_DEVICE_ERROR;
  }

  Status = Bc250SmuSendQueue3Message (BC250_Q3_MSG_SMN_WRITE32, 1, &Value, &Rsp);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: SMU write32 rejected: %r (rsp=0x%08x)\n", Status, Rsp));
    return Status;
  }
  if (Rsp != BC250_SMU_RETURN_OK) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: SMU write32 returned 0x%08x\n", Rsp));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}