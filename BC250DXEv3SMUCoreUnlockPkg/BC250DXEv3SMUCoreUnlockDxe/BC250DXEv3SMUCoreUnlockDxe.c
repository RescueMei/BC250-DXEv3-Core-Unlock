#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <BC250ColdBoot.h>
#include <MeiMeiDXEv3ConfigVar.h>
#include <MeiMeiDXEv3CoreUnlockProtocol.h>

#include "Bc250SmuMailbox.h"

//
// This DXE driver is the only component that manipulates the core presence
// mask (SMN 0x5A870). It consumes the MeiMeiDXEv3CoreVar configuration
// published by the BC250-DXEv3-Menu-Driver and writes the desired mask
// through the SMU's protected mem64 window (Queue 3 messages 0x2B/0x2C),
// mirroring the mailbox sequence of the bc250-smu-unlock reference scripts.
//
// The SMU's protected functions must be available for a mask write. Opening
// them is the responsibility of the separate SMU-unlock driver; this driver
// only probes availability (Queue 3 message 0x2A).
//
// Factory-mask acquisition uses a one-shot cold boot so a clean, untouched
// mask can be captured and persisted for the "disabled -> restore" path:
//
//   1. Factory mask record absent or invalid and learn flag clear:
//        - persist the learn flag
//        - set the volatile MeiMeiDXEv3RequestColdBoot message (one-shot;
//          consumed by the BC250-DXEv3-Cold-Boot driver)
//        - return without touching the mask
//   2. Next boot (cold): learn flag set:
//        - sample the current mask (clean: we are the only manipulator and
//          have not written yet this boot) and persist it as the factory mask
//        - clear the learn flag
//        - fall through to the normal write logic
//   3. Factory mask record already present and valid:
//        - clear any stale learn flag and use the normal write logic
//
// The core presence mask persists across warm resets (only a cold reset
// returns it to the factory value), so the SMN register may hold a mask this
// driver applied on a previous boot. The stored factory record is therefore
// validated by its own CRC-32 fingerprint of the mask byte, never by comparing
// the live register, which would false-positive after any mask write.
//
// Desired mask by configuration:
//   disabled -> stored factory mask
//   enabled  -> 0xFF (all cores)
//   custom   -> per-core mask from the configuration, but only when that mask
//               satisfies the CCX0/CCX1 rules below. An invalid custom mask is
//               rejected: CoreUnlock is persisted as disabled and the known
//               factory mask is written instead.
//
// Custom-mask validity (cores 0-3 = CCX0, cores 4-7 = CCX1):
//   * CCX0 must always have at least one core enabled.
//   * CCX1 must have the same number of cores enabled as CCX0, unless CCX1
//     has zero cores enabled.
// A warm reset is issued only when the register differs from the desired mask.
//
//
// GUID for the shared MeiMeiDXEv3 configuration variables.
//
EFI_GUID  gMeiMeiDXEv3ConfigVarGuid = \
  { 0x49CC168D, 0xE8B0, 0x4613, { 0xA8, 0x07, 0x16, 0x96, 0x99, 0x86, 0x72, 0x6F } };

//
// One-shot cold-boot request namespace, as defined by the BC250-DXEv3-Cold-Boot
// driver's public consumer header.
//
STATIC EFI_GUID  gBc250ColdBootVendorGuid = BC250_COLD_BOOT_VENDOR_GUID;

//
// Message the cold-boot driver displays during the recovery countdown. The
// payload is a NUL-terminated CHAR16 string; an empty payload would select the
// cold-boot driver's default text instead.
//
STATIC CONST CHAR16  mColdBootRequestMessage[] =
  L"Learning factory core mask; performing one-time cold reboot.";

#define MEIMEIDXEV3_DELAY_AFTER_WRITE_US  50000U

//
// CCX membership of the eight cores in the core presence mask.
//   CCX0: cores 0-3 -> bits 0-3
//   CCX1: cores 4-7 -> bits 4-7
//
#define BC250_CCX0_CORE_BITS  0x0FU
#define BC250_CCX1_CORE_BITS  0xF0U

//
// Core-unlock protocol instance published so other DXE drivers can read the
// learned factory core mask. Installed unconditionally in the entry point;
// GetFactoryMask re-reads and CRC-validates the persisted factory record on
// every call.
//
STATIC MEIMEIDXEV3_CORE_UNLOCK_PROTOCOL  mCoreUnlockProtocol;
STATIC EFI_HANDLE                        mCoreUnlockProtocolHandle = NULL;

/**
  Read the MeiMeiDXEv3CoreVar configuration.

  If the variable is absent, apply the driver defaults: core unlock disabled
  and all per-core toggles cleared.

  @param[out] Config  Receives the core configuration.

  @retval EFI_SUCCESS  The configuration was read or defaulted.
**/
STATIC
EFI_STATUS
ReadCoreConfig (
  OUT CORE_CONFIG  *Config
  )
{
  EFI_STATUS  Status;
  UINTN       DataSize;

  DataSize = sizeof (CORE_CONFIG);
  ZeroMem (Config, sizeof (CORE_CONFIG));

  Status = gRT->GetVariable (
                  MEIMEIDXEV3_CORE_VAR_NAME,
                  &gMeiMeiDXEv3ConfigVarGuid,
                  NULL,
                  &DataSize,
                  Config
                  );
  if (Status == EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: %s is larger than CORE_CONFIG\n", "MeiMeiDXEv3CoreVar"));
    return EFI_BUFFER_TOO_SMALL;
  }

  if (EFI_ERROR (Status)) {
    ZeroMem (Config, sizeof (CORE_CONFIG));
    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}

/**
  Read a one-byte non-volatile variable owned by this driver.

  @param[in]  VariableName  Variable name.
  @param[out] Value         Value read, or 0 when absent.
  @param[out] Found         TRUE when the variable exists.

  @retval EFI_SUCCESS  The variable was read or was absent.
  @retval others       GetVariable() failed in a non-EFI_NOT_FOUND way.
**/
STATIC
EFI_STATUS
ReadSingleByteVar (
  IN  CHAR16    *VariableName,
  OUT UINT8     *Value,
  OUT BOOLEAN   *Found
  )
{
  EFI_STATUS  Status;
  UINTN       DataSize;

  *Value = 0;
  *Found = FALSE;
  DataSize = sizeof (UINT8);

  Status = gRT->GetVariable (
                  VariableName,
                  &gMeiMeiDXEv3ConfigVarGuid,
                  NULL,
                  &DataSize,
                  Value
                  );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: failed to read %s: %r\n", VariableName, Status));
    return Status;
  }

  *Found = TRUE;
  return EFI_SUCCESS;
}

/**
  Write a one-byte non-volatile variable owned by this driver.

  @param[in]  VariableName  Variable name.
  @param[in]  Value         Value to write.

  @retval EFI_STATUS  Result of SetVariable().
**/
STATIC
EFI_STATUS
WriteSingleByteVar (
  IN CHAR16  *VariableName,
  IN UINT8   Value
  )
{
  return gRT->SetVariable (
                VariableName,
                &gMeiMeiDXEv3ConfigVarGuid,
                MEIMEIDXEV3_CONFIG_VAR_ATTRIBUTES,
                sizeof (UINT8),
                &Value
                );
}

/**
  Read the persisted factory-mask record.

  @param[out] Record  Factory record read, zeroed when absent.
  @param[out] Found   TRUE when the record variable exists.

  @retval EFI_SUCCESS  The record was read, or was absent.
  @retval others       GetVariable() failed in a non-EFI_NOT_FOUND way.
**/
STATIC
EFI_STATUS
ReadFactoryRecord (
  OUT FACTORY_MASK_RECORD  *Record,
  OUT BOOLEAN              *Found
  )
{
  EFI_STATUS  Status;
  UINTN       DataSize;

  ZeroMem (Record, sizeof (FACTORY_MASK_RECORD));
  *Found = FALSE;
  DataSize = sizeof (FACTORY_MASK_RECORD);

  Status = gRT->GetVariable (
                  MEIMEIDXEV3_FACTORY_MASK_VAR_NAME,
                  &gMeiMeiDXEv3ConfigVarGuid,
                  NULL,
                  &DataSize,
                  Record
                  );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }

  if (Status == EFI_BUFFER_TOO_SMALL) {
    //
    // Older 1-byte factory value from a previous version: treat as absent so
    // the one-shot cold boot re-learns the full record.
    //
    DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: factory-mask variable too small, re-learning\n"));
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: failed to read factory-mask record: %r\n", Status));
    return Status;
  }

  *Found = TRUE;
  return EFI_SUCCESS;
}

/**
  Write the persisted factory-mask record.

  @param[in]  Record  Factory record to write.

  @retval EFI_STATUS  Result of SetVariable().
**/
STATIC
EFI_STATUS
WriteFactoryRecord (
  IN FACTORY_MASK_RECORD  *Record
  )
{
  return gRT->SetVariable (
                MEIMEIDXEV3_FACTORY_MASK_VAR_NAME,
                &gMeiMeiDXEv3ConfigVarGuid,
                MEIMEIDXEV3_CONFIG_VAR_ATTRIBUTES,
                sizeof (FACTORY_MASK_RECORD),
                Record
                );
}

/**
  Persist the core configuration to MeiMeiDXEv3CoreVar.

  Used to set CoreUnlock back to DISABLED when an invalid custom mask is
  rejected. The full 9-byte structure is rewritten so the menu driver's
  variable layout is preserved.

  @param[in]  Config  Core configuration to persist.

  @retval EFI_STATUS  Result of SetVariable().
**/
STATIC
EFI_STATUS
WriteCoreConfig (
  IN CONST CORE_CONFIG  *Config
  )
{
  return gRT->SetVariable (
                MEIMEIDXEV3_CORE_VAR_NAME,
                &gMeiMeiDXEv3ConfigVarGuid,
                MEIMEIDXEV3_CONFIG_VAR_ATTRIBUTES,
                sizeof (CORE_CONFIG),
                (VOID *)Config
                );
}

/**
  Compute a CRC-32 fingerprint of a core-mask value, over its 4-byte
  little-endian representation. The factory record stores this fingerprint of
  the captured mask byte so the record can be validated on later boots without
  reading the live SMN register.

  @param[in]  OriginalValue  Core-mask value (mask in the low byte).

  @return  CRC-32 of the little-endian 4-byte representation.
**/
STATIC
UINT32
ComputeMaskFingerprint (
  IN UINT32  OriginalValue
  )
{
  UINT8  Bytes[4];

  Bytes[0] = (UINT8)(OriginalValue & 0xFFU);
  Bytes[1] = (UINT8)((OriginalValue >> 8) & 0xFFU);
  Bytes[2] = (UINT8)((OriginalValue >> 16) & 0xFFU);
  Bytes[3] = (UINT8)((OriginalValue >> 24) & 0xFFU);

  return CalculateCrc32 (Bytes, sizeof (Bytes));
}

/**
  Request a one-shot cold boot so the clean factory mask can be learned.

  Creates the volatile MeiMeiDXEv3RequestColdBoot variable consumed by the
  BC250-DXEv3-Cold-Boot driver. The payload is the CHAR16 message shown in the
  driver's recovery countdown box. The variable is volatile, so it is cleared
  by the reset and cannot loop.

  @retval EFI_STATUS  Result of SetVariable().
**/
STATIC
EFI_STATUS
RequestColdBoot (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gRT->SetVariable (
                  BC250_COLD_BOOT_VARIABLE_NAME,
                  &gBc250ColdBootVendorGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS,
                  sizeof (mColdBootRequestMessage),
                  (VOID *)mColdBootRequestMessage
                  );
  DEBUG ((DEBUG_INFO, "BC250DXEv3SMUCoreUnlock: cold-boot request message write=%r\n", Status));
  return Status;
}

/**
  Learn the factory core mask, using a one-shot cold boot when needed.

  A stored record is valid only when the mask byte matches its CRC-32
  fingerprint. The check is self-contained: the live SMN register is never
  compared because, after a warm reset, it may hold a mask this driver applied
  on a previous boot rather than the factory value. An absent, undersized, or
  inconsistent record triggers a one-shot cold boot that captures the clean
  factory mask.

  @param[out] FactoryMask   Learned factory mask (low byte).
  @param[out] HaveFactory   TRUE when a factory mask is known.
  @param[out] ColdBooted    TRUE when a cold boot was requested this boot.

  @retval EFI_STATUS  Result of the variable operations.
**/
STATIC
EFI_STATUS
LearnFactoryMask (
  OUT UINT8    *FactoryMask,
  OUT BOOLEAN  *HaveFactory,
  OUT BOOLEAN  *ColdBooted
  )
{
  EFI_STATUS            Status;
  FACTORY_MASK_RECORD   Record;
  BOOLEAN               HaveRecord;
  BOOLEAN               RecordValid;
  UINT8                 LearnFlag;
  BOOLEAN               HaveLearnFlag;
  UINT32                MaskValue;

  *FactoryMask = 0;
  *HaveFactory = FALSE;
  *ColdBooted  = FALSE;

  Status = ReadFactoryRecord (&Record, &HaveRecord);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = ReadSingleByteVar (MEIMEIDXEV3_LEARN_FLAG_VAR_NAME, &LearnFlag, &HaveLearnFlag);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Record-integrity check: the stored mask byte must match its CRC-32
  // fingerprint. The live register is intentionally not compared because,
  // after a warm reset, it may hold a mask this driver applied on a previous
  // boot rather than the factory value.
  //
  RecordValid = FALSE;
  if (HaveRecord) {
    if (ComputeMaskFingerprint ((UINT32)Record.FactoryMask) == Record.OriginalCrc32) {
      RecordValid = TRUE;
    } else {
      DEBUG ((DEBUG_WARN,
              "BC250DXEv3SMUCoreUnlock: factory record hash mismatch (mask=0x%02x, crc=0x%08x), re-learning\n",
              Record.FactoryMask, Record.OriginalCrc32));
    }
  }

  if (RecordValid) {
    //
    // Factory already learned. Clear any stale learn flag and use it.
    //
    if (HaveLearnFlag && LearnFlag != 0) {
      Status = WriteSingleByteVar (MEIMEIDXEV3_LEARN_FLAG_VAR_NAME, 0);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: failed to clear learn flag: %r\n", Status));
      }
    }

    *FactoryMask = Record.FactoryMask;
    *HaveFactory = TRUE;
    return EFI_SUCCESS;
  }

  if (HaveLearnFlag && LearnFlag != 0) {
    //
    // This is the cold boot after the one-shot request. The mask has not been
    // touched yet this boot and we are the only manipulator, so it is the
    // clean factory mask. Capture it together with a CRC-32 fingerprint of the
    // mask byte so the record is self-validating on later boots.
    //
    MaskValue = Bc250SmnRead32 (BC250_CORE_MASK_REG);
    Record.FactoryMask   = (UINT8)(MaskValue & 0xFFU);
    Record.Reserved[0]   = 0;
    Record.Reserved[1]   = 0;
    Record.Reserved[2]   = 0;
    Record.OriginalCrc32 = ComputeMaskFingerprint ((UINT32)Record.FactoryMask);
    DEBUG ((DEBUG_INFO, "BC250DXEv3SMUCoreUnlock: learning factory mask = 0x%02x (crc=0x%08x)\n",
            Record.FactoryMask, Record.OriginalCrc32));

    Status = WriteFactoryRecord (&Record);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: failed to store factory record: %r\n", Status));
      return Status;
    }

    Status = WriteSingleByteVar (MEIMEIDXEV3_LEARN_FLAG_VAR_NAME, 0);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: failed to clear learn flag: %r\n", Status));
    }

    *FactoryMask = Record.FactoryMask;
    *HaveFactory = TRUE;
    return EFI_SUCCESS;
  }

  //
  // No usable factory record and no learn in progress: start the one-shot
  // cold boot.
  //
  Status = WriteSingleByteVar (MEIMEIDXEV3_LEARN_FLAG_VAR_NAME, 1);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: failed to set learn flag: %r\n", Status));
    return Status;
  }

  Status = RequestColdBoot ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: failed to request cold boot: %r\n", Status));
    return Status;
  }

  *ColdBooted = TRUE;
  return EFI_SUCCESS;
}

/**
  Count the number of enabled cores in a core presence mask byte.

  @param[in]  Mask  Core presence mask low byte.

  @return  Number of set bits (0..8).
**/
STATIC
UINTN
CountCoreBits (
  IN UINT8  Mask
  )
{
  UINTN  Count;
  UINTN  Bit;

  Count = 0;
  for (Bit = 0; Bit < 8; Bit++) {
    if ((Mask & (UINT8)(1U << Bit)) != 0) {
      Count++;
    }
  }

  return Count;
}

/**
  Determine whether a custom core presence mask is a valid CCX0/CCX1
  configuration.

  Cores 0-3 belong to CCX0 (bits 0-3) and cores 4-7 belong to CCX1 (bits 4-7).
  A mask is valid only when:
    * CCX0 has at least one core enabled, and
    * CCX1 has the same number of cores enabled as CCX0, or CCX1 has no
      cores enabled at all.

  @param[in]  Mask  Core presence mask low byte.

  @retval TRUE   The mask is a valid CCX0/CCX1 configuration.
  @retval FALSE  The mask violates the CCX0/CCX1 rules.
**/
STATIC
BOOLEAN
IsCoreMaskValid (
  IN UINT8  Mask
  )
{
  UINTN  Ccx0Count;
  UINTN  Ccx1Count;

  Ccx0Count = CountCoreBits ((UINT8)(Mask & BC250_CCX0_CORE_BITS));
  Ccx1Count = CountCoreBits ((UINT8)(Mask & BC250_CCX1_CORE_BITS));

  //
  // CCX0 must always have at least one core enabled.
  //
  if (Ccx0Count == 0) {
    return FALSE;
  }

  //
  // CCX1 must have the same number of cores enabled as CCX0, unless CCX1
  // has zero cores enabled.
  //
  if (Ccx1Count != 0 && Ccx1Count != Ccx0Count) {
    return FALSE;
  }

  return TRUE;
}

/**
  Assemble the custom core presence mask low byte from the per-core toggles.

  @param[in]  Config  Core configuration.

  @return  Core presence mask low byte built from Core0..Core7.
**/
STATIC
UINT8
ComputeCustomMask (
  IN CONST CORE_CONFIG  *Config
  )
{
  return (UINT8)((Config->Core0      ) |
                 (Config->Core1 << 1 ) |
                 (Config->Core2 << 2 ) |
                 (Config->Core3 << 3 ) |
                 (Config->Core4 << 4 ) |
                 (Config->Core5 << 5 ) |
                 (Config->Core6 << 6 ) |
                 (Config->Core7 << 7 ));
}

/**
  Compute the desired core mask low byte from the core configuration.

  @param[in]  Config       Core configuration.
  @param[in]  FactoryMask  Learned factory mask (used for DISABLED).

  @return  Desired core presence mask low byte.
**/
STATIC
UINT8
ComputeDesiredMask (
  IN CONST CORE_CONFIG  *Config,
  IN UINT8              FactoryMask
  )
{
  UINT8  Desired;

  switch (Config->CoreUnlock) {
    case CORE_UNLOCK_ALL_CORES:
      Desired = BC250_CORE_MASK_FULL;
      break;

    case CORE_UNLOCK_CUSTOM:
      Desired = ComputeCustomMask (Config);
      break;

    case CORE_UNLOCK_DISABLED:
    default:
      //
      // Disabled: restore (and stay at) the learned factory mask.
      //
      Desired = FactoryMask;
      break;
  }

  return Desired;
}

/**
  Apply the desired core mask through the SMU protected window and verify.

  @param[in]  DesiredMask  Desired core presence mask low byte.
  @param[out] MaskChanged  Set to TRUE only when a write was performed and
                           verified. When the register already holds
                           DesiredMask, it stays FALSE and no write is made.

  @retval EFI_SUCCESS       The mask already matched, or the write was applied
                            and verified.
  @retval others            The write could not be applied or verified.
**/
STATIC
EFI_STATUS
ApplyCoreMask (
  IN  UINT8    DesiredMask,
  OUT BOOLEAN  *MaskChanged
  )
{
  EFI_STATUS  Status;
  UINT32      MaskValue;
  UINT8       CurrentMask;

  *MaskChanged = FALSE;

  MaskValue = Bc250SmnRead32 (BC250_CORE_MASK_REG);
  CurrentMask = (UINT8)(MaskValue & 0xFFU);
  DEBUG ((DEBUG_INFO, "BC250DXEv3SMUCoreUnlock: current mask=0x%08x (low 0x%02x), desired 0x%02x\n",
          MaskValue, CurrentMask, DesiredMask));

  if (CurrentMask == DesiredMask) {
    DEBUG ((DEBUG_INFO, "BC250DXEv3SMUCoreUnlock: mask already matches desired, no write\n"));
    return EFI_SUCCESS;
  }

  Status = Bc250SmuWriteSmn (BC250_CORE_MASK_REG, DesiredMask);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: failed to write core mask: %r\n", Status));
    return Status;
  }

  //
  // Give the SMU a moment to settle before verifying.
  //
  MicroSecondDelay (MEIMEIDXEV3_DELAY_AFTER_WRITE_US);

  MaskValue = Bc250SmnRead32 (BC250_CORE_MASK_REG);
  CurrentMask = (UINT8)(MaskValue & 0xFFU);
  DEBUG ((DEBUG_INFO, "BC250DXEv3SMUCoreUnlock: post-write mask=0x%08x (low 0x%02x)\n", MaskValue, CurrentMask));

  if (CurrentMask != DesiredMask) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3SMUCoreUnlock: mask verification failed\n"));
    return EFI_DEVICE_ERROR;
  }

  *MaskChanged = TRUE;
  return EFI_SUCCESS;
}

/**
  Core-unlock protocol: read the learned factory core presence mask.

  Re-reads the persisted factory record and validates its CRC-32 fingerprint
  on every call, matching the driver's own learning check.

  @param[in]  This          Protocol instance pointer.
  @param[out] FactoryMask   Receives the factory core presence mask low byte.

  @retval EFI_SUCCESS       The factory mask is available.
  @retval EFI_NOT_FOUND     No valid factory record yet (learning cold boot
                            pending).
  @retval others            The factory record could not be read.
**/
STATIC
EFI_STATUS
EFIAPI
MeiMeiDxEv3GetFactoryMask (
  IN  MEIMEIDXEV3_CORE_UNLOCK_PROTOCOL  *This,
  OUT UINT8                              *FactoryMask
  )
{
  EFI_STATUS           Status;
  FACTORY_MASK_RECORD  Record;
  BOOLEAN              HaveRecord;
  BOOLEAN              RecordValid;

  if (FactoryMask == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *FactoryMask = 0;

  Status = ReadFactoryRecord (&Record, &HaveRecord);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RecordValid = FALSE;
  if (HaveRecord) {
    if (ComputeMaskFingerprint ((UINT32)Record.FactoryMask) == Record.OriginalCrc32) {
      RecordValid = TRUE;
    }
  }

  if (!RecordValid) {
    return EFI_NOT_FOUND;
  }

  *FactoryMask = Record.FactoryMask;
  return EFI_SUCCESS;
}

/**
  DXE entry point.

  The driver:
    * publishes the core-unlock protocol for reading the factory mask,
    * reads the core configuration,
    * acquires the factory mask (one-shot cold boot when the stored record is
      absent or fails its CRC check),
    * rejects an invalid custom core mask by persisting CoreUnlock as DISABLED
      and falling back to the factory mask,
    * computes the desired mask (factory when disabled, 0xFF when enabled,
      per-core when custom),
    * writes it through the SMU protected window when it differs,
    * issues a warm reset only when the mask was actually changed.

  @param[in] ImageHandle   Standard UEFI image handle.
  @param[in] SystemTable   Standard UEFI system table pointer.

  @retval EFI_SUCCESS        Driver completed its work or safely declined.
  @retval EFI_DEVICE_ERROR   Warm reset was requested but control unexpectedly
                             returned.
**/
EFI_STATUS
EFIAPI
BC250DXEv3SMUCoreUnlockEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS   Status;
  CORE_CONFIG  Config;
  UINT8        FactoryMask;
  BOOLEAN      HaveFactory;
  BOOLEAN      ColdBooted;
  BOOLEAN      MaskChanged;
  UINT8        CustomMask;
  UINT8        DesiredMask;

  DEBUG ((DEBUG_INFO, "BC250DXEv3SMUCoreUnlock: entry\n"));

  //
  // Publish the core-unlock protocol before any early returns so a consumer
  // Depex on the protocol GUID is satisfied on every boot, including the
  // one-shot cold boot that learns the factory mask. GetFactoryMask reports
  // EFI_NOT_FOUND until that cycle has persisted a record.
  //
  if (mCoreUnlockProtocolHandle == NULL) {
    mCoreUnlockProtocol.GetFactoryMask = MeiMeiDxEv3GetFactoryMask;
    Status = gBS->InstallProtocolInterface (
                    &mCoreUnlockProtocolHandle,
                    &gMeiMeiDXEv3CoreUnlockProtocolGuid,
                    EFI_NATIVE_INTERFACE,
                    &mCoreUnlockProtocol
                    );
    if (EFI_ERROR (Status)) {
      // Non-fatal: the mask-write path does not depend on the protocol.
      DEBUG ((DEBUG_ERROR,
              "BC250DXEv3SMUCoreUnlock: failed to install core-unlock protocol: %r\n",
              Status));
    }
  }

  //
  // Read the core configuration.
  //
  Status = ReadCoreConfig (&Config);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: failed to read core config: %r\n", Status));
    return EFI_SUCCESS;
  }

  //
  // Factory-mask acquisition (one-shot cold boot).
  //
  Status = LearnFactoryMask (&FactoryMask, &HaveFactory, &ColdBooted);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: factory-mask learning failed: %r\n", Status));
    return EFI_SUCCESS;
  }

  if (ColdBooted) {
    //
    // Cold boot requested; MeiMeiDXEv3RequestColdBoot is volatile and one-shot.
    // Do nothing else this boot.
    //
    DEBUG ((DEBUG_INFO, "BC250DXEv3SMUCoreUnlock: cold boot requested to learn factory mask\n"));
    return EFI_SUCCESS;
  }

  if (!HaveFactory) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: no factory mask available\n"));
    return EFI_SUCCESS;
  }

  //
  // Reject a custom mask that violates the CCX0/CCX1 rules. The setting is
  // persisted as DISABLED so the driver stays disabled on later boots, and
  // the flow falls through to the disabled path, which writes the known
  // factory mask instead of the invalid custom mask.
  //
  if (Config.CoreUnlock == CORE_UNLOCK_CUSTOM) {
    CustomMask = ComputeCustomMask (&Config);
    if (!IsCoreMaskValid (CustomMask)) {
      DEBUG ((DEBUG_WARN,
              "BC250DXEv3SMUCoreUnlock: invalid custom core mask 0x%02x "
              "(CCX0 must keep >=1 core; CCX1 must match CCX0 or be empty), "
              "reverting CoreUnlock to disabled\n",
              CustomMask));

      Config.CoreUnlock = CORE_UNLOCK_DISABLED;
      Status = WriteCoreConfig (&Config);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR,
                "BC250DXEv3SMUCoreUnlock: failed to persist CoreUnlock=disabled: %r\n",
                Status));
        return EFI_SUCCESS;
      }
    }
  }

  DesiredMask = ComputeDesiredMask (&Config, FactoryMask);

  //
  // Apply and verify the mask.
  //
  Status = ApplyCoreMask (DesiredMask, &MaskChanged);
  if (EFI_ERROR (Status)) {
    //
    // Non-fatal: the SMU-unlock driver may need to open the protected cluster
    // on a future boot, or the mask may already be correct.
    //
    DEBUG ((DEBUG_WARN, "BC250DXEv3SMUCoreUnlock: core mask not applied: %r\n", Status));
    return EFI_SUCCESS;
  }

  if (!MaskChanged) {
    //
    // The register already holds the desired mask (the mask persists across
    // warm resets), so no reset is needed; continue the normal boot.
    //
    DEBUG ((DEBUG_INFO, "BC250DXEv3SMUCoreUnlock: mask already matches desired, no reset\n"));
    return EFI_SUCCESS;
  }

  //
  // A warm reset is required for the new core presence mask to take effect.
  //
  DEBUG ((DEBUG_INFO, "BC250DXEv3SMUCoreUnlock: core mask changed, issuing warm reset\n"));
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);

  return EFI_DEVICE_ERROR;
}