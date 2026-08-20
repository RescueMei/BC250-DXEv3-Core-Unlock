#ifndef __BC250_SMU_MAILBOX_H__
#define __BC250_SMU_MAILBOX_H__

#include <Uefi.h>

//
// SMN indirect access window exposed in PCI config space of the host bridge.
//
#define BC250_SMN_INDEX_OFFSET  0xB8
#define BC250_SMN_DATA_OFFSET   0xBC

//
// Core presence mask register (SMN 0x5A870). Only the low byte is meaningful.
//
#define BC250_CORE_MASK_REG     0x0115A870U
#define BC250_CORE_MASK_FULL    0xFFU
#define BC250_FACTORY_MASK_6C   0x77U

//
// SMU Queue 3 mailbox registers (SMN addresses).
//
#define BC250_Q3_CMD            0x03B10A20U
#define BC250_Q3_RSP            0x03B10A80U
#define BC250_Q3_ARG            0x03B10A88U

//
// Queue 3 messages.
//
#define BC250_Q3_MSG_ALIVE              0x01U
#define BC250_Q3_MSG_SMN_READ32         0x2AU
#define BC250_Q3_MSG_SET_SMN_WRITE_ADDR 0x2BU
#define BC250_Q3_MSG_SMN_WRITE32        0x2CU

//
// SMU mailbox response values.
//
#define BC250_SMU_RETURN_OK               0x01U
#define BC250_SMU_RETURN_FAILED           0xFFU
#define BC250_SMU_RETURN_UNKNOWN_CMD      0xFEU
#define BC250_SMU_RETURN_REJECTED_PREREQ  0xFDU
#define BC250_SMU_RETURN_REJECTED_BUSY    0xFCU

//
// Polling parameters, matching the reference scripts.
//
#define BC250_MAILBOX_POLL_DELAY_US  2000U
#define BC250_MAILBOX_TIMEOUT_US     5000000U

/**
  Read a 32-bit value from an SMN register through the host bridge PCI config
  index/data pair.

  @param[in]  Register  SMN register address.

  @return  32-bit value read from the addressed SMN register.
**/
UINT32
Bc250SmnRead32 (
  IN UINT32  Register
  );

/**
  Write a 32-bit value to an SMN register through the host bridge PCI config
  index/data pair.

  @param[in]  Register  SMN register address.
  @param[in]  Value     32-bit value to write.
**/
VOID
Bc250SmnWrite32 (
  IN UINT32  Register,
  IN UINT32  Value
  );

/**
  Determine whether an SMU Queue 3 response value means the queue is idle or the
  last command has completed.

  The reference scripts treat 0x01, 0xFF, 0xFE, 0xFD, and 0xFC as terminal
  response states.

  @param[in]  Status  Raw SMU queue response register value.

  @retval TRUE   Status is one of the known terminal states.
  @retval FALSE  Status is still busy or otherwise non-terminal.
**/
BOOLEAN
Bc250SmuIsDoneStatus (
  IN UINT32  Status
  );

/**
  Send a command through SMU Queue 3 using the mailbox order used by the
  reference implementation (wait for idle, clear RSP, write arguments, write
  command, wait for completion).

  @param[in]  Command   Queue 3 message opcode.
  @param[in]  Argc      Number of 32-bit arguments (0..6).
  @param[in]  Args      Argument values, at least Argc entries.
  @param[out] Status    Terminal Q3_RSP value after the send completes.

  @retval EFI_SUCCESS       The queue reached a terminal state both before and
                            after send.
  @retval EFI_TIMEOUT       The queue never reached a terminal state in time.
**/
EFI_STATUS
Bc250SmuSendQueue3Message (
  IN  UINT32  Command,
  IN  UINTN   Argc,
  IN  UINT32  *Args,
  OUT UINT32  *Status
  );

/**
  Determine whether the SMU's protected (secure) functions are available.

  Probes with Queue 3 message 0x2A. If the SMU replies with
  SMU_RETURN_REJECTED_PREREQ (0xFD), the gated cluster is closed and protected
  functions are unavailable.

  @retval TRUE   Protected functions are available.
  @retval FALSE  Protected functions are unavailable.
**/
BOOLEAN
Bc250SmuSecureAccessEnabled (
  VOID
  );

/**
  Write a 32-bit value to an SMN register through the SMU's protected
  mem64 window. Requires protected functions to be available.

  @param[in]  Address  SMN register address.
  @param[in]  Value    32-bit value to write.

  @retval EFI_SUCCESS       The write was accepted.
  @retval EFI_DEVICE_ERROR  The protected SMU write was rejected or timed out.
**/
EFI_STATUS
Bc250SmuWriteSmn (
  IN UINT32  Address,
  IN UINT32  Value
  );

#endif