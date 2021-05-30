/*++

Module Name:

    device.c - Device handling events for example driver.

Abstract:

   This file contains the device entry points and callbacks.
    
Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceSpiKmCreateDevice)
#endif

NTSTATUS
AmtPtpDeviceSpiKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_OBJECT_ATTRIBUTES DeviceAttributes;
	WDF_OBJECT_ATTRIBUTES TimerAttributes;
    PDEVICE_CONTEXT pDeviceContext;
	WDF_TIMER_CONFIG TimerConfig;
    WDFDEVICE Device;
    NTSTATUS Status;

	WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;

    PAGED_CODE();

	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Entry"
	);

	// Initialize Power Callback
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

	// Initialize PNP power event callbacks
	pnpPowerCallbacks.EvtDevicePrepareHardware = AmtPtpEvtDevicePrepareHardware;
	pnpPowerCallbacks.EvtDeviceD0Entry = AmtPtpEvtDeviceD0Entry;
	pnpPowerCallbacks.EvtDeviceD0Exit = AmtPtpEvtDeviceD0Exit;
	pnpPowerCallbacks.EvtDeviceSelfManagedIoInit = AmtPtpEvtDeviceSelfManagedIoInitOrRestart;
	pnpPowerCallbacks.EvtDeviceSelfManagedIoRestart = AmtPtpEvtDeviceSelfManagedIoInitOrRestart;
	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	// Create WDF device object
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&DeviceAttributes, DEVICE_CONTEXT);

    Status = WdfDeviceCreate(&DeviceInit, &DeviceAttributes, &Device);

    if (NT_SUCCESS(Status)) 
	{
        //
        // Get a pointer to the device context structure that we just associated
        // with the device object. We define this structure in the device.h
        // header file. DeviceGetContext is an inline function generated by
        // using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
        // This function will do the type checking and return the device context.
        // If you pass a wrong object handle it will return NULL and assert if
        // run under framework verifier mode.
        //
        pDeviceContext = DeviceGetContext(Device);

		//
		// Put itself in
		//
		pDeviceContext->SpiDevice = Device;

		//
		// Create a list of buffers
		//
		Status = WdfLookasideListCreate(
			WDF_NO_OBJECT_ATTRIBUTES,
			REPORT_BUFFER_SIZE,
			NonPagedPoolNx,
			WDF_NO_OBJECT_ATTRIBUTES,
			PTP_LIST_POOL_TAG,
			&pDeviceContext->HidReadBufferLookaside
		);

		if (!NT_SUCCESS(Status)) {
			TraceEvents(
				TRACE_LEVEL_INFORMATION,
				TRACE_DRIVER,
				"%!FUNC! WdfLookasideListCreate failed with %!STATUS!",
				Status
			);
			goto exit;
		}

		//
		// Create power-on recovery timer
		//
		WDF_TIMER_CONFIG_INIT(&TimerConfig, AmtPtpPowerRecoveryTimerCallback);
		TimerConfig.AutomaticSerialization = TRUE;
		WDF_OBJECT_ATTRIBUTES_INIT(&TimerAttributes);
		TimerAttributes.ParentObject = Device;
		TimerAttributes.ExecutionLevel = WdfExecutionLevelPassive;
		Status = WdfTimerCreate(&TimerConfig, &TimerAttributes, &pDeviceContext->PowerOnRecoveryTimer);

		//
		// Retrieve IO target.
		//
		pDeviceContext->SpiTrackpadIoTarget = WdfDeviceGetIoTarget(Device);
		if (pDeviceContext->SpiTrackpadIoTarget == NULL) 
		{
			Status = STATUS_INVALID_DEVICE_STATE;
			goto exit;
		}

		//
		// Reset power status.
		//
		pDeviceContext->DeviceStatus = D3;

        //
        // Create a device interface so that applications can find and talk
        // to us.
        //
        Status = WdfDeviceCreateDeviceInterface(
            Device,
            &GUID_DEVINTERFACE_AmtPtpDeviceSpiKm,
            NULL // ReferenceString
        );

        if (NT_SUCCESS(Status)) 
		{
            //
            // Initialize the I/O Package and any Queues
            //
            Status = AmtPtpDeviceSpiKmQueueInitialize(Device);
        }
    }

exit:
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Exit, Status = %!STATUS!",
		Status
	);

    return Status;
}

NTSTATUS
AmtPtpEvtDevicePrepareHardware(
	_In_ WDFDEVICE Device,
	_In_ WDFCMRESLIST ResourceList,
	_In_ WDFCMRESLIST ResourceListTranslated
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDeviceContext;

	WDF_MEMORY_DESCRIPTOR HidAttributeMemoryDescriptor;
	HID_DEVICE_ATTRIBUTES DeviceAttributes;
	
	const SPI_TRACKPAD_INFO* pTrackpadInfo;
	BOOLEAN DeviceFound = FALSE;

	WDFKEY ParamRegistryKey;
	DECLARE_CONST_UNICODE_STRING(DesiredReportTypeKey, L"DesiredReportType");
	ULONG DesiredReportTypeValue, Length, ValueType = 0;

	PAGED_CODE();
	UNREFERENCED_PARAMETER(ResourceList);
	UNREFERENCED_PARAMETER(ResourceListTranslated);

	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Entry"
	);

	pDeviceContext = DeviceGetContext(Device);
	if (pDeviceContext == NULL)
	{
		Status = STATUS_INVALID_DEVICE_STATE;
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! pDeviceContext == NULL"
		);

		goto exit;
	}

	// Request device attribute descriptor for self-identification.
	RtlZeroMemory(&DeviceAttributes, sizeof(DeviceAttributes));
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&HidAttributeMemoryDescriptor,
		(PVOID) &DeviceAttributes,
		sizeof(DeviceAttributes)
	);

	Status = WdfIoTargetSendInternalIoctlSynchronously(
		pDeviceContext->SpiTrackpadIoTarget,
		NULL,
		IOCTL_HID_GET_DEVICE_ATTRIBUTES,
		NULL,
		&HidAttributeMemoryDescriptor,
		NULL,
		NULL
	);

	if (!NT_SUCCESS(Status))
	{
		KdPrintEx((
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_INFO_LEVEL,
			"WdfIoTargetSendInternalIoctlSynchronously failed, status = 0x%x \n",
			Status
		));

		goto exit;
	}

	pDeviceContext->HidVendorID = DeviceAttributes.VendorID;
	pDeviceContext->HidProductID = DeviceAttributes.ProductID;
	pDeviceContext->HidVersionNumber = DeviceAttributes.VersionNumber;

	// Find proper metadata in HID registry
	for (pTrackpadInfo = SpiTrackpadConfigTable; pTrackpadInfo->VendorId; ++pTrackpadInfo)
	{
		if (pTrackpadInfo->VendorId == DeviceAttributes.VendorID &&
			pTrackpadInfo->ProductId == DeviceAttributes.ProductID)
		{
			pDeviceContext->TrackpadInfo.ProductId = pTrackpadInfo->ProductId;
			pDeviceContext->TrackpadInfo.VendorId = pTrackpadInfo->VendorId;
			pDeviceContext->TrackpadInfo.XMin = pTrackpadInfo->XMin;
			pDeviceContext->TrackpadInfo.XMax = pTrackpadInfo->XMax;
			pDeviceContext->TrackpadInfo.YMin = pTrackpadInfo->YMin;
			pDeviceContext->TrackpadInfo.YMax = pTrackpadInfo->YMax;

			DeviceFound = TRUE;
			break;
		}
	}

	if (!DeviceFound)
	{
		Status = STATUS_NOT_FOUND;
		goto exit;
	}

	// Check the desired report type.
	Status = WdfDriverOpenParametersRegistryKey(
		WdfDeviceGetDriver(Device),
		KEY_READ,
		WDF_NO_OBJECT_ATTRIBUTES,
		&ParamRegistryKey
	);

	if (NT_SUCCESS(Status))
	{
		Status = WdfRegistryQueryValue(
			ParamRegistryKey,
			&DesiredReportTypeKey,
			sizeof(ULONG),
			&DesiredReportTypeValue,
			&Length,
			&ValueType
		);

		if (NT_SUCCESS(Status))
		{
			switch (DesiredReportTypeValue)
			{
			case 0:
				pDeviceContext->ReportType = PrecisionTouchpad;
				break;
			case 1:
				pDeviceContext->ReportType = Touchscreen;
				break;
			default:
				Status = STATUS_INVALID_PARAMETER;
				break;
			}
		}

		WdfRegistryClose(ParamRegistryKey);
	}

	// We don't really care if that param read fails.
	Status = STATUS_SUCCESS;

exit:
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Exit, Status = %!STATUS!",
		Status
	);

	return Status;
}

NTSTATUS
AmtPtpEvtDeviceD0Entry(
	_In_ WDFDEVICE Device,
	_In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDeviceContext;

	// Log status
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Entry - coming from %s",
		DbgDevicePowerString(PreviousState)
	);

	pDeviceContext = DeviceGetContext(Device);

	// We will configure the device in Self Managed IO init / restart routine
	pDeviceContext->DeviceStatus = D0ActiveAndUnconfigured;

	// Set time
	KeQueryPerformanceCounter(&pDeviceContext->LastReportTime);

	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! <-- AmtPtpDeviceEvtDeviceD0Entry"
	);

	return Status;
}

NTSTATUS
AmtPtpEvtDeviceD0Exit(
	_In_ WDFDEVICE Device,
	_In_ WDF_POWER_DEVICE_STATE TargetState
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDeviceContext;
	WDFREQUEST OutstandingRequest;

	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Exit - moving to %s",
		DbgDevicePowerString(TargetState)
	);

	pDeviceContext = DeviceGetContext(Device);
	pDeviceContext->DeviceStatus = D3;

	// Cancel all outstanding requests
	while (NT_SUCCESS(Status)) {
		Status = WdfIoQueueRetrieveNextRequest(
			pDeviceContext->HidQueue, 
			&OutstandingRequest
		);

		if (NT_SUCCESS(Status)) {
			WdfRequestComplete(OutstandingRequest, STATUS_CANCELLED);
		}
	}

	// When the queue is empty, this is expected
	Status = STATUS_SUCCESS;

	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! <--AmtPtpDeviceEvtDeviceD0Exit"
	);

	return Status;
}

NTSTATUS
AmtPtpEvtDeviceSelfManagedIoInitOrRestart(
	_In_ WDFDEVICE Device
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDeviceContext;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
	pDeviceContext = DeviceGetContext(Device);
	
	Status = AmtPtpSpiSetState(Device, TRUE);
	if (!NT_SUCCESS(Status))
	{
		// In this case, we will retry after 5 seconds. Block any incoming requests.
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "%!FUNC! AmtPtpSpiSetState failed with %!STATUS!. Retry after 5 seconds", Status);
		Status = STATUS_SUCCESS;
		WdfTimerStart(pDeviceContext->PowerOnRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(5));
		goto exit;
	}
	else
	{
		// Set time and status
		pDeviceContext->DeviceStatus = D0ActiveAndConfigured;
		KeQueryPerformanceCounter(&pDeviceContext->LastReportTime);
	}

exit:
	TraceEvents(TRACE_LEVEL_INFORMATION,TRACE_DRIVER, "%!FUNC! Exit, Status = %!STATUS!", Status);
	return Status;
}

PCHAR
DbgDevicePowerString(
	_In_ WDF_POWER_DEVICE_STATE Type
)
{
	switch (Type)
	{
	case WdfPowerDeviceInvalid:
		return "WdfPowerDeviceInvalid";
	case WdfPowerDeviceD0:
		return "WdfPowerDeviceD0";
	case WdfPowerDeviceD1:
		return "WdfPowerDeviceD1";
	case WdfPowerDeviceD2:
		return "WdfPowerDeviceD2";
	case WdfPowerDeviceD3:
		return "WdfPowerDeviceD3";
	case WdfPowerDeviceD3Final:
		return "WdfPowerDeviceD3Final";
	case WdfPowerDevicePrepareForHibernation:
		return "WdfPowerDevicePrepareForHibernation";
	case WdfPowerDeviceMaximum:
		return "WdfPowerDeviceMaximum";
	default:
		return "UnKnown Device Power State";
	}
}

NTSTATUS
AmtPtpSpiSetState(
	_In_ WDFDEVICE Device,
	_In_ BOOLEAN DesiredState
)
{
	NTSTATUS Status;
	PDEVICE_CONTEXT pDeviceContext;
	UCHAR HidPacketBuffer[HID_XFER_PACKET_SIZE];
	WDF_MEMORY_DESCRIPTOR HidMemoryDescriptor;
	PHID_XFER_PACKET pHidPacket;
	PSPI_SET_FEATURE pSpiSetStatus;

	pDeviceContext = DeviceGetContext(Device);
	if (pDeviceContext == NULL)
	{
		Status = STATUS_INVALID_DEVICE_STATE;
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! pDeviceContext == NULL"
		);

		goto exit;
	}

	RtlZeroMemory(HidPacketBuffer, sizeof(HidPacketBuffer));
	pHidPacket = (PHID_XFER_PACKET) &HidPacketBuffer;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&HidMemoryDescriptor,
		(PVOID) &HidPacketBuffer,
		HID_XFER_PACKET_SIZE
	);

	pHidPacket->reportId = HID_REPORTID_MOUSE;
	pHidPacket->reportBufferLen = sizeof(SPI_SET_FEATURE);
	pHidPacket->reportBuffer = (PUCHAR) pHidPacket + sizeof(HID_XFER_PACKET);
	pSpiSetStatus = (PSPI_SET_FEATURE) pHidPacket->reportBuffer;

	// SPI Bus, location 2
	pSpiSetStatus->BusLocation = 2;
	pSpiSetStatus->Status = DesiredState ? 1 : 0;

	// Will non-internal IOCTL work?
	Status = WdfIoTargetSendInternalIoctlSynchronously(
		pDeviceContext->SpiTrackpadIoTarget,
		NULL,
		IOCTL_HID_SET_FEATURE,
		&HidMemoryDescriptor,
		NULL,
		NULL,
		NULL
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfIoTargetSendIoctlSynchronously failed with %!STATUS!",
			Status
		);
	}
	else
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! Changed trackpad status to %d",
			DesiredState
		);
	}

exit:
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Exit, Status = %!STATUS!",
		Status
	);

	return Status;
}

void AmtPtpPowerRecoveryTimerCallback(
	WDFTIMER Timer
)
{
	WDFDEVICE Device;
	PDEVICE_CONTEXT pDeviceContext;
	NTSTATUS Status = STATUS_SUCCESS;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
	Device = WdfTimerGetParentObject(Timer);
	pDeviceContext = DeviceGetContext(Device);

	Status = AmtPtpSpiSetState(Device, TRUE);
	if (NT_SUCCESS(Status))
	{
		// Triage request and set status
		AmtPtpSpiInputIssueRequest(Device);
		pDeviceContext->DeviceStatus = D0ActiveAndConfigured;
	}

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit, Status = %!STATUS!", Status);
}
