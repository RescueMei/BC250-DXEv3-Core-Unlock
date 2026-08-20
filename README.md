# BC250-DXEv3-Core-Unlock

My v3 SMU Core Unlock DXE Driver. Uses arbitrary writes to the core mask
through the SMU's protected mem64 window (Queue 3 messages `0x2B`/`0x2C`) instead
of the previous exploit that could only write `0xFF`.

## Package

`BC250DXEv3SMUCoreUnlockPkg/` builds the DXE module `MeiMeiDXEv3_SMU_CoreUnlock`.

This driver is the only component that manipulates the core presence mask
(SMN `0x5A870`). It consumes the `MeiMeiDXEv3CoreVar` configuration published by
the `BC250-DXEv3-Menu-Driver`:

| CoreUnlock | Action |
| --- | --- |
| `DISABLED` (0) | Restore the learned factory mask. |
| `ALL_CORES` (1) | Write `0xFF` (all 8 cores). |
| `CUSTOM` (2) | Write the per-core mask from `Core0`..`Core7`, but only if it is a valid CCX0/CCX1 configuration (see below); otherwise fall back to `DISABLED`. |

## Custom core mask validation

A custom mask is only applied when it is a valid CCX0/CCX1 configuration.
Cores 0-3 belong to CCX0 (mask bits 0-3) and cores 4-7 belong to CCX1 (mask
bits 4-7):

* CCX0 must always have at least one core enabled.
* CCX1 must have the same number of cores enabled as CCX0, unless CCX1 has zero
  cores enabled (all of CCX1 off is always valid).

When the custom mask violates these rules, the driver persists `CoreUnlock` as
`DISABLED` and writes the learned factory mask instead, so the system always
boots to a sane core configuration.

Opening the SMU's protected functions is handled by the separate SMU-unlock
driver; this driver only probes availability (Queue 3 message `0x2A`) and skips
the write when the protected cluster is closed.

## Factory-mask learning (one-shot cold boot)

A clean factory mask is needed for the "disabled -> restore" path. The driver
captures it with a one-shot cold boot:

1. No valid factory mask known (record absent, undersized, or failing its CRC
   check) -> set the non-volatile learn flag and the volatile
   `MeiMeiDXEv3RequestColdBoot` message (consumed by the
   `BC250-DXEv3-Cold-Boot` driver), then stop.
2. On the cold boot the SMU state is clean: sample the current mask and persist
   it as `MeiMeiDXEv3CoreFactoryMaskVar`, clear the learn flag, then run the
   normal write logic.

The `MeiMeiDXEv3RequestColdBoot` variable is volatile, so it cannot loop. Its
payload is a `CHAR16` message displayed in the cold-boot driver's recovery
countdown box; this driver supplies `"Learning factory core mask; performing
one-time cold reboot."` so the user can see why the board is cycling once.

## Variables

| Variable | Size | Purpose |
| --- | --- | --- |
| `MeiMeiDXEv3CoreVar` | 9 B | Core-unlock configuration (menu driver). |
| `MeiMeiDXEv3CoreFactoryMaskVar` | 8 B | `FACTORY_MASK_RECORD`: learned factory mask (low byte) + CRC-32 hash of the original 32-bit mask register value. |
| `MeiMeiDXEv3LearnFactoryFlag` | 1 B | One-shot cold-boot learn marker. |

All use vendor GUID `49CC168D-E8B0-4613-A807-16969986726F` with
`BOOTSERVICE | RUNTIME | NON_VOLATILE` attributes.

The factory-mask record is validated by its own CRC-32 fingerprint of the
stored mask byte; the live SMN register is never compared, because the core
presence mask persists across warm resets and may hold a mask the driver
applied on a previous boot. If the stored mask and its CRC do not match (or no
valid record exists), a one-shot cold boot captures the clean factory mask.
The desired mask is the stored factory mask when disabled, `0xFF` when
enabled, or the per-core custom mask when it is a valid CCX0/CCX1
configuration (an invalid custom mask falls back to `DISABLED` and the factory
mask); a warm reset is issued only when the current mask differs from the
desired mask.

## Protocol

The driver publishes `MEIMEIDXEV3_CORE_UNLOCK_PROTOCOL` so other DXE drivers
can read the learned factory core mask at runtime:

| Item | Value |
| --- | --- |
| GUID | `gMeiMeiDXEv3CoreUnlockProtocolGuid` (`687319be-5bc5-44a7-8e60-e5f1e5fd0168`) |
| Header | `<MeiMeiDXEv3CoreUnlockProtocol.h>` |
| Service | `GetFactoryMask (This, OUT UINT8 *FactoryMask)` |

`GetFactoryMask` re-reads `MeiMeiDXEv3CoreFactoryMaskVar` and validates its
CRC-32 fingerprint on every call, so the returned value always matches the
mask the driver itself uses. It returns `EFI_NOT_FOUND` until the one-shot
cold-boot learning cycle has persisted a record.

The protocol is installed unconditionally at the top of the entry point, so a
consumer may add it to its `[Depex]` on every boot:

```ini
[Depex]
  gMeiMeiDXEv3CoreUnlockProtocolGuid
```

Consumer lookup:

```c
#include <Library/UefiBootServicesTableLib.h>
#include <MeiMeiDXEv3CoreUnlockProtocol.h>

MEIMEIDXEV3_CORE_UNLOCK_PROTOCOL  *Protocol;
UINT8                              FactoryMask;

Status = gBS->LocateProtocol (&gMeiMeiDXEv3CoreUnlockProtocolGuid, NULL,
                              (VOID **)&Protocol);
if (EFI_ERROR (Status)) {
  // core-unlock driver not available
}
Status = Protocol->GetFactoryMask (Protocol, &FactoryMask);
```

A consumer also needs `UefiBootServicesTableLib` in its `[LibraryClasses]`, the
package `BC250DXEv3SMUCoreUnlockPkg.dec` in its `[Packages]`, and the protocol
GUID in its `[Protocols]`:

```ini
[Packages]
  MdePkg/MdePkg.dec
  BC250DXEv3SMUCoreUnlockPkg/BC250DXEv3SMUCoreUnlockPkg.dec

[LibraryClasses]
  UefiBootServicesTableLib

[Protocols]
  gMeiMeiDXEv3CoreUnlockProtocolGuid

[Depex]
  gMeiMeiDXEv3CoreUnlockProtocolGuid
```

## Build

```bash
./build_ffs.sh
```

A container build (podman) produces:

- `Build/Output/MeiMeiDXEv3_SMU_CoreUnlock.efi`
- `Build/Output/MeiMeiDXEv3_SMU_CoreUnlock.ffs`

For a host build, set `NO_CONTAINER=1` and `EDK2_DIR=/path/to/edk2`.

The `.ffs` can be inserted into an AMI DXE firmware volume with UEFITool.

## Reference

- `Ref/BC250-DXEv2-SMU-Core-Unlock`: the v2 driver this is based on.
- `Ref/bc250-smu-unlock`: the Linux userspace SMU unlock / arbitrary read-write
  reference (`set_core_mask.py`).