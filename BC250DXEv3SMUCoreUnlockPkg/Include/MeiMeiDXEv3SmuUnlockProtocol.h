/**
  Vendored definition of the BC-250 DXEv3 SMU secure-access unlock protocol.

  The GUID is declared in this package's .dec ([Protocols]); this header
  provides the callable Grant() method so a one-shot factory-learning path can
  request privileged SMU access even when the SmuUnlock variable is disabled.
  Keep this in sync with BC250DXEv3SMUUnlockPkg/Include/MeiMeiDXEv3SmuUnlockProtocol.h.
**/
#ifndef __MEIMEIDXEV3_SMU_UNLOCK_PROTOCOL_H__
#define __MEIMEIDXEV3_SMU_UNLOCK_PROTOCOL_H__

#include <Uefi.h>

typedef struct _MEIMEIDXEV3_SMU_UNLOCK_PROTOCOL  MEIMEIDXEV3_SMU_UNLOCK_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *MEIMEIDXEV3_SMU_UNLOCK_GRANT)(
  IN MEIMEIDXEV3_SMU_UNLOCK_PROTOCOL  *This
  );

struct _MEIMEIDXEV3_SMU_UNLOCK_PROTOCOL {
  UINT32                        Revision;
  MEIMEIDXEV3_SMU_UNLOCK_GRANT  Grant;
};

#endif
