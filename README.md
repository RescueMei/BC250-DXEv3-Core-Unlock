# BC250-DXEv3-Core-Unlock

Core-unlock DXE driver for the BC-250. It writes the CPU core presence mask
through the SMU's protected mem64 window (Queue 3 messages `0x2B`/`0x2C`),
which allows arbitrary per-core selection instead of the older all-cores-only
write. It also learns and publishes the factory core mask so a reset can always
restore a sane configuration.

> **Firmware warning:** this driver writes SMU-protected state and may request a
> cold boot. Keep a recovery path before enabling it.

## At a glance

| Item | Value |
| --- | --- |
| Package / module | `BC250DXEv3SMUCoreUnlockPkg` -> `MeiMeiDXEv3_SMU_Core_Unlock.efi` |
| Depex | `gEfiVariableArchProtocolGuid AND gMeiMeiDXEv3SmuUnlockProtocolGuid` |
| Produces | `gMeiMeiDXEv3CoreUnlockProtocolGuid` (`GetFactoryMask`) |
| Consumes | SMU-unlock protocol, `MeiMeiDXEv3CoreVar`, cold-boot request variable |
| Configuration | `MeiMeiDXEv3CoreVar`, 9 bytes |
| SMU SRAM | none (SMN mask write through Q3) |
| Compliance | [COMPLIANCE.md](COMPLIANCE.md) |

## Dependencies

`BC250-DXEv3-SMU-Unlock` must have installed its protocol; the Depex enforces
that. The one-shot learning path asks that driver for privileged access through
`Grant()`. A cold boot is requested through the `BC250-DXEv3-Cold-Boot` variable
contract, not by calling that driver directly.

## How it works

The mask lives in SMN `0x0115A870`; the driver reads it, compares it with the
desired value, writes through the Q3 window, delays briefly, and reads it back
to confirm. A warm reset is issued only when the mask actually changed.

Mode selection:

| `CoreUnlock` | Action |
| --- | --- |
| `DISABLED` (0) | Restore the learned factory mask. |
| `ALL_CORES` (1) | Write `0xFF` (all eight cores). |
| `CUSTOM` (2) | Write the per-core mask from `Core0`..`Core7`, if it is a valid CCX0/CCX1 configuration; otherwise persist `DISABLED` and restore the factory mask. |

CCX rules: cores 0-3 are CCX0, cores 4-7 are CCX1. CCX0 must keep at least one
core enabled. CCX1 must enable either zero cores or the same number as CCX0.
An invalid custom mask is never applied.

### Factory-mask learning

A clean factory mask is required for the disabled/restore path:

1. If no valid record exists (absent, undersized, or failing its CRC), the
   driver sets the non-volatile learn flag and writes the volatile
   `MeiMeiDXEv3RequestColdBoot` message with a human-readable reason, then stops
   feature work for this boot.
2. `BC250-DXEv3-Cold-Boot` performs the cold boot.
3. On the clean boot the driver samples the current mask, persists it with a
   CRC-32 fingerprint, clears the learn flag, and continues with the normal
   write logic.

Learning runs before any mode-dependent work, so it also happens when core
unlock is disabled. The live register is deliberately never used as the factory
reference: it persists across warm resets and may already hold an applied mask.

## Configuration

Shared `CORE_CONFIG` record:

- Header: `BC250-DXEv3-Shared/MeiMeiDXEv3SharedPkg/Include/Config/MeiMeiDXEv3Config.h`
- Variable: `MEIMEIDXEV3_CORE_VAR_NAME` (`MeiMeiDXEv3CoreVar`)
- Fields: `CoreUnlock`, `Core0`..`Core7`

A missing or unreadable variable means "all zeros" (disabled). An oversized or
wrong-attribute variable produces no mask write and the boot continues.

Driver-owned persisted records (same vendor GUID, same attributes):

| Variable | Size | Purpose |
| --- | --- | --- |
| `MeiMeiDXEv3CoreFactoryMaskVar` | 8 B | Learned mask plus CRC-32 fingerprint. |
| `MeiMeiDXEv3LearnFactoryFlag` | 1 B | One-shot cold-boot learn marker. |

## Protocols

Also publishes `gMeiMeiDXEv3DriverInfoProtocolGuid` with its name, version, module GUID, config schema, and payload version for the `BC250-DXEv3-Driver-Info` telemetry driver.

`gMeiMeiDXEv3CoreUnlockProtocolGuid`
(`687319BE-5BC5-44A7-8E60-E5F1E5FD0168`), header
`<MeiMeiDXEv3CoreUnlockProtocol.h>`.

`GetFactoryMask()` re-reads and CRC-validates the stored record on every call,
so it always agrees with the mask the driver uses. It returns `EFI_NOT_FOUND`
until learning has persisted a record. The protocol is installed
unconditionally at the top of the entry point.

```c
#include <Library/UefiBootServicesTableLib.h>
#include <MeiMeiDXEv3CoreUnlockProtocol.h>

MEIMEIDXEV3_CORE_UNLOCK_PROTOCOL  *Core;
UINT8                              FactoryMask;

Status = gBS->LocateProtocol (&gMeiMeiDXEv3CoreUnlockProtocolGuid, NULL,
                              (VOID **)&Core);
if (!EFI_ERROR (Status)) {
  Status = Core->GetFactoryMask (Core, &FactoryMask);
}
```

A consumer needs `BC250DXEv3SMUCoreUnlockPkg.dec` in `[Packages]`, the GUID in
`[Protocols]`, and the protocol GUID in `[Depex]` if it must wait.

## SMU SRAM placement

No lane in the shared map. The driver only uses the Q3 SMN window. See
[SMU_MEMORY_MAP.md](https://github.com/RescueMei/BC250-DXEv3-Shared/blob/main/docs/SMU_MEMORY_MAP.md).

## Build

```bash
./build_ffs.sh                                  # container build
NO_CONTAINER=1 EDK2_DIR=/path/to/edk2 ./build_ffs.sh   # host build
```

Output:

- `Build/Output/MeiMeiDXEv3_SMU_Core_Unlock.efi`
- `Build/Output/MeiMeiDXEv3_SMU_Core_Unlock.ffs`

## Validation

2026-11-24, eight-driver stack, configured ceiling 2100 MHz: `DEPLOY_OK`,
actual clock 2100 MHz sustained, rail voltage <= 1018 mV, 193 W GPU peak, 82 C
peak, 0 ring timeouts, 0 C2P timeouts, board reachable after the workload. See
[COMPLIANCE.md](COMPLIANCE.md).

## Credits

Based on `set_core_mask.py` from
[rw-r-r-0644/bc250-smu-unlock](https://github.com/rw-r-r-0644/bc250-smu-unlock).
