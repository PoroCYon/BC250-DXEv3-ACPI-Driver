#include <Uefi.h>

#include <Guid/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PciSegmentLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/AcpiTable.h>

#include "AcpiTableBlobs.h"

//
// This DXE driver injects ACPI tables for the active number of physical cores.
//
// Driver execution is controlled by the shared "ACPI Patch" option (the
// AcpiPatch field of MeiMeiDXEv3AcpiVar) exposed by the BC250-DXEv3-Menu-Driver.
//
// Operational flow to unlock cores:
//   1. Read the menu driver's MeiMeiDXEv3AcpiVar variable and require the
//      AcpiPatch field to be non-zero. Exit if disabled or unavailable.
//   2. Read SMN 0x0115A870 through the host bridge SMN index/data window.
//   3. Popcount the low byte. Firmware compacts enabled SMT threads into P000..Pxxx.
//   4. Install the common tables and one pair table for each enabled core.
//

// Host bridge PCI location used to access AMD's SMN index/data window.
#define BC250_HOST_PCI_SEGMENT      0
#define BC250_HOST_PCI_BUS          0
#define BC250_HOST_PCI_DEVICE       0
#define BC250_HOST_PCI_FUNCTION     0

// PCI configuration-space offsets that expose the SMN indirect access window.
#define SMN_INDEX_OFFSET            0xB8
#define SMN_DATA_OFFSET             0xBC

// Core-mask register containing one enable bit per physical core.
#define MASK_REG                    0x0115A870U

//
// Shared DXEv3 configuration variable owned by the BC250-DXEv3-Menu-Driver.
// Only the single-byte AcpiPatch field (offset 0) is consumed.
//
#define MEIMEIDXEV3_ACPI_VAR_NAME   L"MeiMeiDXEv3AcpiVar"

typedef struct {
  UINT8  AcpiPatch;
} ACPI_CONFIG;

STATIC EFI_GUID mMeiMeiDXEv3ConfigVarGuid = {
  0x49CC168D, 0xE8B0, 0x4613, { 0xA8, 0x07, 0x16, 0x96, 0x99, 0x86, 0x72, 0x6F }
};

#define BC250_PCI_SEGMENT_ADDRESS(Offset) \
  PCI_SEGMENT_LIB_ADDRESS (BC250_HOST_PCI_SEGMENT, BC250_HOST_PCI_BUS, BC250_HOST_PCI_DEVICE, BC250_HOST_PCI_FUNCTION, (Offset))

// Tracks the protocol notification event and remembers whether this boot has
// already had its ACPI override tables installed.
typedef struct {
  EFI_EVENT   Event;
  VOID        *Registration;
  BOOLEAN     TablesInstalled;
  UINTN       InstalledKeyCount;
  UINTN       InstalledKeys[BC250_MAX_ACPI_TABLES];
} BC250_ACPI_AUTO_INJECT_CONTEXT;

// Single global context for this one-shot DXE driver instance.
STATIC BC250_ACPI_AUTO_INJECT_CONTEXT mAcpiInjectContext = {
  NULL,
  NULL,
  FALSE,
  0,
  { 0 }
};

/**
  Read a 32-bit value from an SMN register through the host bridge PCI config
  index/data pair.

  @param[in] Register  SMN register address.

  @return 32-bit value read from the addressed SMN register.
**/
STATIC
UINT32
SmnRead32 (
  IN UINT32  Register
  )
{
  PciSegmentWrite32 (BC250_PCI_SEGMENT_ADDRESS (SMN_INDEX_OFFSET), Register);
  return PciSegmentRead32 (BC250_PCI_SEGMENT_ADDRESS (SMN_DATA_OFFSET));
}

STATIC
UINT8
CountEnabledCores (
  IN UINT8  Mask
  )
{
  UINT8 Count;

  Count = 0;
  while (Mask != 0) {
    Count++;
    Mask = (UINT8)(Mask & (UINT8)(Mask - 1));
  }
  return Count;
}

/**
  Read the ACPI Patch gate from the shared MeiMeiDXEv3AcpiVar variable.

  The variable is owned by the BC250-DXEv3-Menu-Driver.  When the variable is
  absent the menu driver's defaults (all zeros) apply, so this driver treats a
  missing variable as disabled and fails closed.

  @param[out] AcpiPatchEnabled  TRUE when the AcpiPatch field is non-zero.

  @retval EFI_SUCCESS  The gate was read (AcpiPatchEnabled reflects the setting).
  @retval others       The variable is absent or malformed (caller treats as disabled).
**/
STATIC
EFI_STATUS
GetAcpiPatchEnabled (
  OUT BOOLEAN  *AcpiPatchEnabled
  )
{
  EFI_STATUS   Status;
  UINTN        DataSize;
  ACPI_CONFIG  Config;

  *AcpiPatchEnabled = FALSE;
  ZeroMem (&Config, sizeof (Config));

  DataSize = sizeof (Config);
  Status = gRT->GetVariable (
                  MEIMEIDXEV3_ACPI_VAR_NAME,
                  &mMeiMeiDXEv3ConfigVarGuid,
                  NULL,
                  &DataSize,
                  &Config
                  );
  if (Status == EFI_NOT_FOUND) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: %s not found, ACPI patch disabled\n",
            MEIMEIDXEV3_ACPI_VAR_NAME));
    return EFI_NOT_FOUND;
  }

  if (Status == EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: %s smaller than ACPI_CONFIG, ACPI patch disabled\n",
            MEIMEIDXEV3_ACPI_VAR_NAME));
    return EFI_BAD_BUFFER_SIZE;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: failed to read %s: %r\n",
            MEIMEIDXEV3_ACPI_VAR_NAME, Status));
    return Status;
  }

  *AcpiPatchEnabled = (BOOLEAN)(Config.AcpiPatch != 0U);
  DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: AcpiPatch=%u\n", Config.AcpiPatch));
  return EFI_SUCCESS;
}

/**
  Install a selected set of embedded ACPI table blobs.

  Each blob is handed directly to EFI_ACPI_TABLE_PROTOCOL and, on success, the
  returned table key is saved in the driver context for diagnostic visibility.

  @param[in]     AcpiTable   Located ACPI table protocol instance.
  @param[in]     Blobs       Array of ACPI blobs to install.
  @param[in]     BlobCount   Number of entries in Blobs.
  @param[in,out] Context     Driver state used to record installed table keys.

  @retval EFI_SUCCESS  All tables were installed successfully.
  @retval others       A table installation failed.
**/
STATIC
EFI_STATUS
InstallAcpiBlobSet (
  IN EFI_ACPI_TABLE_PROTOCOL   *AcpiTable,
  IN CONST BC250_ACPI_BLOB     *Blobs,
  IN UINTN                     BlobCount,
  IN BC250_ACPI_AUTO_INJECT_CONTEXT *Context
  )
{
  EFI_STATUS Status;
  UINTN      Index;
  UINTN      TableKey;

  for (Index = 0; Index < BlobCount; ++Index) {
    TableKey = 0;
    Status = AcpiTable->InstallAcpiTable (
                          AcpiTable,
                          (VOID *)Blobs[Index].Data,
                          Blobs[Index].Size,
                          &TableKey
                          );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: failed to install %a: %r\n", Blobs[Index].Name, Status));
      return Status;
    }

    DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: installed %a (%u bytes), key=%u\n", Blobs[Index].Name, (UINT32)Blobs[Index].Size, (UINT32)TableKey));
    if (Context->InstalledKeyCount < ARRAY_SIZE (Context->InstalledKeys)) {
      Context->InstalledKeys[Context->InstalledKeyCount++] = TableKey;
    }
  }

  return EFI_SUCCESS;
}

/**
  ACPI table protocol notification callback.

  Once EFI_ACPI_TABLE_PROTOCOL is available, this callback re-checks the
  firmware ACPI-patch gate, reads the BC250 core mask, selects the compacted
  processor-pair tables, and installs the corresponding AML tables exactly once
  for the current boot.

  @param[in] Event       Notification event being signaled.
  @param[in] ContextPtr  Pointer to BC250_ACPI_AUTO_INJECT_CONTEXT.
**/
STATIC
VOID
EFIAPI
OnAcpiTableProtocolReady (
  IN EFI_EVENT Event,
  IN VOID      *ContextPtr
  )
{
  EFI_STATUS                    Status;
  EFI_ACPI_TABLE_PROTOCOL       *AcpiTable;
  UINT32                        MaskValue;
  UINT8                         MaskLowByte;
  UINT8                         EnabledCoreCount;
  UINT8                         PairIndex;
  BC250_ACPI_BLOB               SelectedBlobs[BC250_MAX_ACPI_TABLES];
  UINTN                         SelectedCount;
  BOOLEAN                       AcpiPatchEnabled;
  BC250_ACPI_AUTO_INJECT_CONTEXT *Context;

  Context = (BC250_ACPI_AUTO_INJECT_CONTEXT *)ContextPtr;
  (VOID)Event;
  if (Context->TablesInstalled) {
    return;
  }

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: ACPI table protocol not yet available: %r\n", Status));
    return;
  }

  AcpiPatchEnabled = FALSE;
  Status = GetAcpiPatchEnabled (&AcpiPatchEnabled);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ACPIAutoInjectDxe: ACPI patch gate unavailable, skipping\n"));
    return;
  }

  if (!AcpiPatchEnabled) {
    DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: ACPI patch is disabled, skipping\n"));
    return;
  }

  // Read the hardware mask only after the firmware gate allows injection.
  MaskValue = SmnRead32 (MASK_REG);
  MaskLowByte = (UINT8)(MaskValue & 0xFFU);
  DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: core presence mask=0x%08x\n", MaskValue));

  EnabledCoreCount = CountEnabledCores (MaskLowByte);
  if ((EnabledCoreCount == 0) || (EnabledCoreCount > BC250_PAIR_TABLE_COUNT)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: invalid enabled-core count=%u\n", EnabledCoreCount));
    return;
  }

  SelectedCount = 0;
  SelectedBlobs[SelectedCount++] = gPstateCommonTable;
  if (EnabledCoreCount == BC250_PAIR_TABLE_COUNT) {
    SelectedBlobs[SelectedCount++] = gFullCstateCommonTable;
  } else {
    SelectedBlobs[SelectedCount++] = gPartialCstateCommonTable;
  }

  for (PairIndex = 0; PairIndex < EnabledCoreCount; ++PairIndex) {
    SelectedBlobs[SelectedCount++] = gPstatePairs[PairIndex];
    if (EnabledCoreCount == BC250_PAIR_TABLE_COUNT) {
      SelectedBlobs[SelectedCount++] = gFullCstatePairs[PairIndex];
    } else {
      SelectedBlobs[SelectedCount++] = gPartialCstatePairs[PairIndex];
    }
  }

  if (EnabledCoreCount < BC250_PAIR_TABLE_COUNT) {
    SelectedBlobs[SelectedCount++] = gPtswakTable;
  }

  DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: selected %u physical cores (%u tables)\n", EnabledCoreCount, (UINT32)SelectedCount));
  Status = InstallAcpiBlobSet (AcpiTable, SelectedBlobs, SelectedCount, Context);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: ACPI installation aborted: %r\n", Status));
    return;
  }

  Context->TablesInstalled = TRUE;
  if (Context->Event != NULL) {
    gBS->CloseEvent (Context->Event);
    Context->Event = NULL;
    Context->Registration = NULL;
  }
}

/**
  DXE entry point.

  The driver registers for EFI_ACPI_TABLE_PROTOCOL notification so it can defer
  SSDT installation until the firmware ACPI table interface is ready. It also
  invokes the callback immediately to handle the common case where the protocol
  already exists.

  @param[in] ImageHandle   Standard UEFI image handle.
  @param[in] SystemTable   Standard UEFI system table pointer.

  @retval EFI_SUCCESS  Driver initialization completed successfully.
  @retval others       Event creation or protocol-notify registration failed.
**/
EFI_STATUS
EFIAPI
BC250DXEv3ACPIAutoInjectEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  DEBUG ((DEBUG_INFO, "BC250DXEv3ACPIAutoInjectDxe: entry\n"));

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnAcpiTableProtocolReady,
                  &mAcpiInjectContext,
                  &mAcpiInjectContext.Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: failed to create notify event: %r\n", Status));
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiAcpiTableProtocolGuid,
                  mAcpiInjectContext.Event,
                  &mAcpiInjectContext.Registration
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BC250DXEv3ACPIAutoInjectDxe: failed to register ACPI notify: %r\n", Status));
    gBS->CloseEvent (mAcpiInjectContext.Event);
    mAcpiInjectContext.Event = NULL;
    return Status;
  }

  OnAcpiTableProtocolReady (mAcpiInjectContext.Event, &mAcpiInjectContext);
  return EFI_SUCCESS;
}