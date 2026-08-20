/**
  Public contract between the requester and the BC-250 DXEv3 cold-boot driver.

  A requester creates BC250_COLD_BOOT_VARIABLE_NAME with attributes
  EFI_VARIABLE_BOOTSERVICE_ACCESS under the vendor GUID below.  The payload is
  an optional NUL-terminated CHAR16 message that the cold-boot driver displays
  while performing the reboot countdown.  An empty or malformed payload selects
  the driver's default message.

  The variable is volatile, so it exists only for the current boot session and
  is cleared by reset or power loss.  The driver consumes (deletes) it at
  ReadyToBoot, making the request one-shot.
**/

#ifndef __BC250_COLD_BOOT_H__
#define __BC250_COLD_BOOT_H__

// Volatile variable name used as the one-shot cold-boot request marker.  The
// payload is a NUL-terminated CHAR16 message to display, or empty for the
// driver's default text.
#define BC250_COLD_BOOT_VARIABLE_NAME  L"MeiMeiDXEv3RequestColdBoot"

// Vendor GUID shared by all MeiMeiDXEv3 configuration variables.
#define BC250_COLD_BOOT_VENDOR_GUID \
  { 0x49cc168d, 0xe8b0, 0x4613, { 0xa8, 0x07, 0x16, 0x96, 0x99, 0x86, 0x72, 0x6f } }

#endif