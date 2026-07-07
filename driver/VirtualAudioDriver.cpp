/**
 * ============================================================================
 * Windows AVStream Virtual Audio Device Driver (Educational Reference)
 * ============================================================================
 *
 * This file contains a template implementation of a Windows kernel-mode 
 * virtual audio driver using the AVStream (KS) architecture.
 *
 * AVStream is the standard Microsoft architecture for building hardware and 
 * virtual streaming devices (microphones, webcams, speakers).
 *
 * Core Concept:
 * 1. The driver registers a Filter Descriptor with one Input Pin (virtual speaker)
 *    and one Output Pin (virtual microphone).
 * 2. When the user sends audio to the virtual speaker, the Process callback 
 *    captures the AVStream Leading Edge stream pointers.
 * 3. It copies the samples directly from the input stream pointer into a 
 *    shared kernel ring buffer or loops it back directly into the output pin queue.
 * 4. Other applications (like Discord) reading from the virtual microphone will 
 *    pull frames directly from this loopback queue.
 *
 * Requirements for Building:
 * - Visual Studio 2022 + C++ Desktop Workload
 * - Windows Driver Kit (WDK) 10/11
 * - Enterprise WDK (EWDK) can also be used.
 *
 * Requirements for Installation:
 * - Windows 64-bit kernel mandates EV Code Signing.
 * - For local testing, run: `bcdedit /set testsigning on` and reboot, which 
 *   allows loading self-signed or test-signed certificates (.sys).
 */

#include <ksoverlay.h>
#include <portcls.h>
#include <ksmedia.h>

// Unique GUIDs for our virtual filter and pins
// {4B6E1F4B-9E33-4DFA-A3C9-122B43D5F5A1}
static const GUID GUID_VirtualAudioFilter = 
{ 0x4b6e1f4b, 0x9e33, 0x4dfa, { 0xa3, 0xc9, 0x12, 0x2b, 0x43, 0xd5, 0xf5, 0xa1 } };

// --- Pin Formats (48kHz, 16-bit Stereo PCM) ---
static const KS_DATARANGE_AUDIO PinAudioRanges[] = {
    {
        {
            sizeof(KS_DATARANGE_AUDIO),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        2,      // Max Channels
        16,     // Min Bits per Sample
        16,     // Max Bits per Sample
        48000,  // Min Frequency (Hz)
        48000   // Max Frequency (Hz)
    }
};

static const PKSDATARANGE PinAudioRangesPtrs[] = {
    (PKSDATARANGE)&PinAudioRanges[0]
};

// --- Process Callback (The Real-Time Kernel Pipeline) ---
/**
 * This is the real-time processing callback for the AVStream filter.
 * AVStream triggers this callback whenever there is data available in the input 
 * queue (virtual speaker) and space available in the output queue (virtual mic).
 */
NTSTATUS VirtualAudioFilterProcess(
    IN PKSFILTER Filter,
    IN PKSPROCESSPIN_INDEXENTRY ProcessPinsIndex
) {
    // 1. Locate the input and output process pins
    PKSPROCESSPIN inputPin = ProcessPinsIndex[0].Pins[0];  // Index 0: Render (Speaker)
    PKSPROCESSPIN outputPin = ProcessPinsIndex[1].Pins[0]; // Index 1: Capture (Mic)

    if (!inputPin || !outputPin) {
        return STATUS_SUCCESS; // Not enough pins ready to process
    }

    // 2. Obtain leading edge pointers
    PKSSTREAM_POINTER pInStream = inputPin->StreamPointer;
    PKSSTREAM_POINTER pOutStream = outputPin->StreamPointer;

    if (!pInStream || !pOutStream) {
        return STATUS_SUCCESS;
    }

    // 3. Process loopback buffer (Zero-Copy or direct copy)
    ULONG bytesToCopy = min(pInStream->OffsetOut.Remaining, pOutStream->OffsetOut.Remaining);

    if (bytesToCopy > 0) {
        PUCHAR pInputBuffer = (PUCHAR)pInStream->OffsetOut.Data;
        PUCHAR pOutputBuffer = (PUCHAR)pOutStream->OffsetOut.Data;

        // Perform loopback injection
        RtlCopyMemory(pOutputBuffer, pInputBuffer, bytesToCopy);

        // Advance stream pointers
        KsStreamPointerAdvanceOffsets(pInStream, 0, bytesToCopy, TRUE);
        KsStreamPointerAdvanceOffsets(pOutStream, 0, bytesToCopy, TRUE);
    }

    return STATUS_SUCCESS;
}

// --- Pin Descriptors ---
static const KSPIN_DISPATCH PinDispatch = {
    NULL,          // Create
    NULL,          // Close
    NULL,          // Process (We use filter-centric processing instead of pin-centric)
    NULL,          // Reset
    NULL,          // SetDataFormat
    NULL,          // GetNotificationStructure
    NULL,          // IntersectDataFormat
    NULL           // Resolution
};

static const KSPIN_DESCRIPTOR_EX PinDescriptors[] = {
    // Pin 0: Virtual Speaker (Input/Sink)
    {
        &PinDispatch,
        NULL,
        {
            0, NULL, 0, NULL, // Interfaces & Mediums (Defaults)
            SIZEOF_ARRAY(PinAudioRangesPtrs),
            PinAudioRangesPtrs,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_BOTH,
            &GUID_VirtualAudioFilter,
            NULL, 0
        },
        KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING, // Flags
        1, // Instances maximum
        1, // Instances minimum
        NULL, // Allocator framing
        NULL  // IntersectHandler
    },
    // Pin 1: Virtual Microphone (Output/Source)
    {
        &PinDispatch,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(PinAudioRangesPtrs),
            PinAudioRangesPtrs,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_BOTH,
            &GUID_VirtualAudioFilter,
            NULL, 0
        },
        0,
        1,
        1,
        NULL,
        NULL
    }
};

// --- Filter Dispatch & Descriptor ---
static const KSFILTER_DISPATCH FilterDispatch = {
    NULL,                       // Create
    NULL,                       // Close
    VirtualAudioFilterProcess,  // Process
    NULL                        // Reset
};

static const KSFILTER_DESCRIPTOR FilterDescriptor = {
    &FilterDispatch,
    NULL,                       // AutomationTable
    KSFILTER_DESCRIPTOR_VERSION,
    KSFILTER_FLAG_CRITICAL_PROCESSING, // Flags (Runs in high priority kernel thread)
    &GUID_VirtualAudioFilter,
    SIZEOF_ARRAY(PinDescriptors),
    PinDescriptors
};

// --- Device Dispatch & Entry Point ---
static const KSDEVICE_DISPATCH DeviceDispatch = {
    NULL, // Add
    NULL, // Start
    NULL, // PostStart
    NULL, // Stop
    NULL, // Remove
    NULL, // QueryPower
    NULL, // SetPower
    NULL, // QueryInterface
};

static const KSDEVICE_DESCRIPTOR DeviceDescriptor = {
    &DeviceDispatch,
    0,
    NULL
};

/**
 * Driver Entry Point. Called when Windows loads the driver into memory.
 */
extern "C" NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
) {
    KdPrint(("VirtualAudioDriver: DriverEntry Called\n"));

    // Register our AVStream hardware-simulated device class
    NTSTATUS status = KsInitializeDriver(
        DriverObject,
        RegistryPath,
        &DeviceDescriptor
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("VirtualAudioDriver: Failed to initialize. Status = 0x%08X\n", status));
    }

    return status;
}
