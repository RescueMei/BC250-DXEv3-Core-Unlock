#ifndef __MEIMEIDXEV3_CONFIG_VAR_H__
#define __MEIMEIDXEV3_CONFIG_VAR_H__

#include <Uefi.h>

//
// Vendor GUID shared by all MeiMeiDXEv3 configuration variables.
// 49CC168D-E8B0-4613-A807-16969986726F
//
extern EFI_GUID  gMeiMeiDXEv3ConfigVarGuid;

#define MEIMEIDXEV3_CONFIG_VAR_ATTRIBUTES \
  (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE)

//
// Config variables maintained by the BC250-DXEv3-Menu-Driver.
//
#define MEIMEIDXEV3_CORE_VAR_NAME          L"MeiMeiDXEv3CoreVar"

//
// Variables owned by this driver (core-mask learning).
//
#define MEIMEIDXEV3_FACTORY_MASK_VAR_NAME  L"MeiMeiDXEv3CoreFactoryMaskVar"
#define MEIMEIDXEV3_LEARN_FLAG_VAR_NAME    L"MeiMeiDXEv3LearnFactoryFlag"

//
// One-shot cold-boot request. The variable name and vendor GUID are defined in
// the public consumer header of the BC250-DXEv3-Cold-Boot driver; see
// <BC250ColdBoot.h> (BC250_COLD_BOOT_VARIABLE_NAME / BC250_COLD_BOOT_VENDOR_GUID).
//

//
// CORE_CONFIG layout (9 bytes), identical to the menu driver definition.
//
typedef struct {
  UINT8  CoreUnlock;
  UINT8  Core0;
  UINT8  Core1;
  UINT8  Core2;
  UINT8  Core3;
  UINT8  Core4;
  UINT8  Core5;
  UINT8  Core6;
  UINT8  Core7;
} CORE_CONFIG;

//
// Factory-mask record persisted by this driver in MeiMeiDXEv3CoreFactoryMaskVar.
// FactoryMask is the low byte of the core presence mask (SMN 0x5A870) captured
// on the clean cold boot; OriginalCrc32 is a CRC-32 fingerprint of that mask
// byte so later boots can validate the record without reading the live SMN
// register (which may hold an applied mask after a warm reset).
//
typedef struct {
  UINT8   FactoryMask;
  UINT8   Reserved[3];
  UINT32  OriginalCrc32;
} FACTORY_MASK_RECORD;

#define CORE_UNLOCK_DISABLED      0
#define CORE_UNLOCK_ALL_CORES     1
#define CORE_UNLOCK_CUSTOM        2

#endif