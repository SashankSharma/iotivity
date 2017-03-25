/* *****************************************************************
 *
 * Copyright 2017 Microsoft
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ******************************************************************/

#include "ipcainternal.h"

using namespace std;
using namespace std::placeholders;

#include "oic_malloc.h"
#include "oic_time.h"
#include "ocapi.h"
#include "pinoxmcommon.h"
#include "srmutility.h"
#include "ocrandom.h"

#define TAG                "IPCA_OcfFramework"
#define DO_DEBUG           0

const unsigned short c_discoveryTimeout = 5;  // Max number of seconds to discover
                                              // security information for a device

// Initialize Persistent Storage for security database
FILE* server_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

OCPersistentStorage ps = {server_fopen, fread, fwrite, fclose, unlink};

OCFFramework::OCFFramework() :
    m_isStarted(false),
    m_isStopping(false)
{
}

OCFFramework::~OCFFramework()
{
}

IPCAStatus OCFFramework::Start(const IPCAAppInfoInternal& appInfo, bool isUnitTestMode)
{
    std::lock_guard<std::mutex> lock(m_startStopMutex);

    if (m_isStarted)
    {
        // it's already started.
        return IPCA_OK;
    }

    PlatformConfig Configuration {
                        ServiceType::InProc,
                        ModeType::Both,  // Server mode is required for security provisioning.
                        "0.0.0.0", // By setting to "0.0.0.0", it binds to all available interfaces
                        0,         // Uses randomly available port
                        QualityOfService::NaQos,
                        &ps};

    OCPlatform::Configure(Configuration);

    // Initialize the database that will be used for provisioning
    if (OCSecure::provisionInit("") != OC_STACK_OK)
    {
        OIC_LOG_V(FATAL, TAG, "Failed provisionInit()");
        return IPCA_FAIL;
    }

    // Device Info.
    char deviceName[256];
    char deviceSoftwareVersion[256];
    char manufacturerName[256];
    OCStringLL types { nullptr, nullptr };  //  no vertical resource type.

    CopyStringToBufferAllowTruncate(
        appInfo.appName, deviceName, ARRAY_SIZE(deviceName));
    CopyStringToBufferAllowTruncate(
        appInfo.appSoftwareVersion, deviceSoftwareVersion, ARRAY_SIZE(deviceSoftwareVersion));
    CopyStringToBufferAllowTruncate(
        appInfo.appCompanyName, manufacturerName, ARRAY_SIZE(manufacturerName));

    OCDeviceInfo deviceInfo = { deviceName, &types, deviceSoftwareVersion, nullptr };

    // Platform Info
    IPCAUuid platformUUID;
    char platformId[UUID_STRING_SIZE] = { 0 };
    char platformManufacturerName[256] = "";
    char manufacturerUrl[256] = "";
    char modelNumber[] = "";
    char dateManufacture[] = "";
    char platformVersion[] = "";
    char osVersion[] = "";
    char hardwareVersion[] = "";
    char firmwareVersion[] = "";
    char supportURL[] = "";

#if defined(_WIN32)
    // @todo: generate per platform UUID (e.g. using input such as hostname).
    platformUUID = {0xd9, 0x9c, 0x23, 0x50, 0xd9, 0x5e, 0x11, 0xe6,
                    0xbf, 0x26, 0xce, 0xc0, 0xc9, 0x32, 0xce, 0x01};
    std::string platformName = "Microsoft";
    std::string platformUrl = "http://www.microsoft.com";
#else
    platformUUID = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
                    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
    std::string platformName = "";
    std::string platformUrl = "";
#endif

    CopyStringToBufferAllowTruncate(platformName,
        platformManufacturerName, ARRAY_SIZE(platformManufacturerName));

    CopyStringToBufferAllowTruncate(platformUrl,
        manufacturerUrl, ARRAY_SIZE(manufacturerUrl));

    OCConvertUuidToString(platformUUID.uuid, platformId);

    OCPlatformInfo platformInfo = {
        platformId,
        platformManufacturerName,
        manufacturerUrl,
        modelNumber,
        dateManufacture,
        platformVersion,
        osVersion,
        hardwareVersion,
        firmwareVersion,
        supportURL,
        nullptr};

    if (!isUnitTestMode)
    {
        if (OC_STACK_OK != OCPlatform::registerPlatformInfo(platformInfo))
        {
            return IPCA_FAIL;
        }

        if (OC_STACK_OK != OCPlatform::registerDeviceInfo(deviceInfo))
        {
            return IPCA_FAIL;
        }
    }

    // Start the worker thread that performs periodic check on device status.
    m_workerThread = std::thread(&OCFFramework::WorkerThread, this);
    m_isStarted = true;
    return IPCA_OK;
}

IPCAStatus OCFFramework::Stop(InputPinCallbackHandle passwordInputCallbackHandle,
                              DisplayPinCallbackHandle passwordDisplayCallbackHandle)
{
    std::lock_guard<std::mutex> lock(m_startStopMutex);

    if (m_isStarted == false)
    {
        // not started yet.
        return IPCA_OK;
    }

    CleanupRequestAccessDevices();

    OCSecure::deregisterInputPinCallback(passwordInputCallbackHandle);
    OCSecure::deregisterDisplayPinCallback(passwordDisplayCallbackHandle);


    m_isStopping = true;

    m_workerThreadCV.notify_all();
    if (m_workerThread.joinable())
    {
        m_workerThread.join();
    }

// @future: OCFFramework can't shut down because there's no cancellation for all underlying apis
// like OCPlatform::findResource, etc.
#if 0
    m_OCFDevices.clear();
    m_OCFDevicesIndexedByDeviceURI.clear();
#endif

    m_isStopping = false;
    m_isStarted = false;

    return IPCA_OK;
}

void OCFFramework::WorkerThread(OCFFramework* ocfFramework)
{
    std::unique_lock<std::mutex> workerThreadLock(ocfFramework->m_workerThreadMutex);

    const size_t WorkerThreadSleepTimeSeconds = 2;
    std::chrono::seconds workerThreadSleepTime(WorkerThreadSleepTimeSeconds);

    while (false == ocfFramework->m_isStopping)
    {
        uint64_t currentTime = OICGetCurrentTime(TIME_IN_MS);
        std::vector<DeviceDetails::Ptr> devicesThatAreNotResponding;
        std::vector<DeviceDetails::Ptr> devicesThatAreNotOpened;
        std::vector<DeviceDetails::Ptr> devicesToGetCommonResources;

        // Collect devices that are not used, i.e. discovered a while back and those that are not
        // used by app for a while.
        {
            std::lock_guard<std::recursive_mutex> lock(ocfFramework->m_OCFFrameworkMutex);
            const unsigned int AllowedTimeSincLastCloseMs = 300000;
            const unsigned int AllowedTimeSinceLastDiscoveryResponseMs = 60000;

            // Walk through each device.
            for (auto const& device : ocfFramework->m_OCFDevices)
            {
                // Is device opened by app?
                if ((device.second->deviceOpenCount == 0) &&
                    (currentTime - device.second->lastCloseDeviceTime > AllowedTimeSincLastCloseMs))
                {
                    devicesThatAreNotOpened.push_back(device.second);
                    continue;  // device details is about to be deleted.
                }

                // Has device responded to Discovery?
                if ((device.second->deviceNotRespondingIndicated == false) &&
                    (currentTime - device.second->lastResponseTimeToDiscovery > AllowedTimeSinceLastDiscoveryResponseMs))
                {
                    device.second->deviceNotRespondingIndicated = true;
                    devicesThatAreNotResponding.push_back(device.second);
                }

                // Are there common resources that are not yet obtained.
                if (!device.second->deviceInfoAvailable ||
                    !device.second->platformInfoAvailable ||
                    !device.second->maintenanceResourceAvailable)
                {
                     devicesToGetCommonResources.push_back(device.second);
                }
            }

            // Erase unopened devices from the m_OCFDevices.
            for (auto& device : devicesThatAreNotOpened)
            {
                for (auto const& deviceUri : ocfFramework->m_OCFDevices[device->deviceId]->deviceUris)
                {
                    ocfFramework->m_OCFDevicesIndexedByDeviceURI.erase(deviceUri);
                }

                ocfFramework->m_OCFDevices.erase(device->deviceId);
                OIC_LOG_V(INFO, TAG, "Device deleted from m_OCFDevices: %s",
                    device->deviceId.c_str());
            }
        }

        // Get common resources.
        for (const auto& device : devicesToGetCommonResources)
        {
            ocfFramework->GetCommonResources(device);
        }

        // Make a snapshot of all callbacks.
        std::vector<Callback::Ptr> callbackSnapshot;
        {
            std::lock_guard<std::recursive_mutex> lock(ocfFramework->m_OCFFrameworkMutex);
            callbackSnapshot = ocfFramework->m_callbacks;
        }

        // Callback to apps.
        for (const auto& device : devicesThatAreNotResponding)
        {
            for (const auto& callback : callbackSnapshot)
            {
                callback->DeviceDiscoveryCallback(
                                        false, /* device is no longer responding to discovery */
                                        false,
                                        device->deviceInfo,
                                        device->discoveredResourceTypes);
            }
        }

        ocfFramework->m_workerThreadCV.wait_for(
                            workerThreadLock,
                            workerThreadSleepTime,
                            [ocfFramework]() { return ocfFramework->m_isStopping; });
    }
}


IPCAStatus OCFFramework::IPCADeviceOpenCalled(std::string& deviceId)
{
    // Has the app discovered the device?
    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return IPCA_DEVICE_NOT_DISCOVERED;
    }

    deviceDetails->deviceOpenCount++;
    return IPCA_OK;
}

IPCAStatus OCFFramework::IPCADeviceCloseCalled(std::string& deviceId)
{
    // Has the app discovered the device?
    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return IPCA_DEVICE_NOT_DISCOVERED;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);
        if (--deviceDetails->deviceOpenCount == 0)
        {
            deviceDetails->lastCloseDeviceTime = OICGetCurrentTime(TIME_IN_MS);
        }
    }

    assert(deviceDetails->deviceOpenCount >= 0);
    return IPCA_OK;
}

IPCAStatus OCFFramework::RegisterAppCallbackObject(Callback::Ptr cb)
{
    std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);
    m_callbacks.push_back(cb);
    return IPCA_OK;
}

void OCFFramework::UnregisterAppCallbackObject(Callback::Ptr cb)
{
    std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);
    for (size_t i = 0 ; i < m_callbacks.size() ; i++)
    {
        if (m_callbacks[i] == cb)
        {
            m_callbacks.erase(m_callbacks.begin() + i);
            break;
        }
    }
}

void OCFFramework::OnResourceFound(std::shared_ptr<OCResource> resource)
{
    bool newDevice = false; // set to true if the resource is from new device.
    bool updatedDeviceInformation = false; // set to true when device information is updated
                                           // (e.g. new resource, new resource type, etc.)

    OIC_LOG_V(INFO, TAG, "OCFFramework::OnResourceFound:  sid: [%s]  uri[%s]",
        resource->sid().c_str(), resource->uri().c_str());

    std::string resourcePath = resource->uri();
    DeviceDetails::Ptr deviceDetails;

    {
        std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);

        // Create new DeviceDetails if it's a newly found device id.
        if (m_OCFDevices.find(resource->sid()) == m_OCFDevices.end())
        {
            // New device.
            newDevice = true;
            deviceDetails = std::shared_ptr<DeviceDetails>(new DeviceDetails());
            if (deviceDetails == nullptr)
            {
                OIC_LOG_V(WARNING, TAG, "OnResourceFound:: out of memory.");
                return; // system is out of memory, this device won't show up in app.
            }

            deviceDetails->deviceId = resource->sid();
            deviceDetails->deviceInfoRequestCount = 0;
            deviceDetails->deviceInfoAvailable = false; // set to true in OnDeviceInfoCallback()
            deviceDetails->platformInfoRequestCount = 0;
            deviceDetails->platformInfoAvailable = false; // set to true in OnPlatformInfoCallback()
            deviceDetails->maintenanceResourceRequestCount = 0;
            deviceDetails->maintenanceResourceAvailable = false;
            deviceDetails->securityInfoAvailable = false; // set to true in
                                                          // RequestAccessWorkerThread()
            deviceDetails->securityInfo.isStarted = false; // set to true in RequestAccess()
            deviceDetails->deviceOpenCount = 0;
            deviceDetails->lastPingTime = 0;

            // Device is not opened at this time.
            deviceDetails->lastCloseDeviceTime = OICGetCurrentTime(TIME_IN_MS);

            // Device ID is known at this time.
            deviceDetails->deviceInfo.deviceId = resource->sid();

            // Add to list of devices.
            m_OCFDevices[resource->sid()] = deviceDetails;

            OIC_LOG_V(INFO, TAG, "Added device ID: [%s]", resource->sid().c_str());
            OIC_LOG_V(INFO, TAG, "m_OCFDevices count = [%d]", m_OCFDevices.size());
        }

        // Populate the details about the device.
        deviceDetails = m_OCFDevices[resource->sid()];

        // Device is discovered.
        deviceDetails->deviceNotRespondingIndicated = false;
        deviceDetails->lastResponseTimeToDiscovery = OICGetCurrentTime(TIME_IN_MS);

        if (deviceDetails->resourceMap.find(resourcePath) == deviceDetails->resourceMap.end())
        {
            updatedDeviceInformation = true;    // new resource.
        }

        // Add (or replace with this latest) resource for the resource path.
        deviceDetails->resourceMap[resourcePath] = resource;

        // Add the device uri if it's new.
        if (std::find(deviceDetails->deviceUris.begin(),
                      deviceDetails->deviceUris.end(),
                      resource->host()) == deviceDetails->deviceUris.end())
        {
            deviceDetails->deviceUris.push_back(resource->host());
            m_OCFDevicesIndexedByDeviceURI[resource->host()] = deviceDetails;
            updatedDeviceInformation = true;    // new device uri.
        }

        // Add the resource types to global list of this device.  Overlapped resource types among
        // resources will be collapsed.
        if (AddNewStringsToTargetList(resource->getResourceTypes(),
                    deviceDetails->discoveredResourceTypes))
        {
            updatedDeviceInformation = true;    // new resource type.
        }

        if (AddNewStringsToTargetList(resource->getResourceInterfaces(),
                    deviceDetails->discoveredResourceInterfaces))
        {
            updatedDeviceInformation = true;    // new resource interface.
        }
    }

    if (newDevice)
    {
        // Discover all the resources of this device.
        DiscoverAllResourcesGivenHost(resource->host());

        // Get device & platform info for the new device URI.
        GetCommonResources(deviceDetails);
    }

    // Inform apps. If new device, the device info may come in subsequent discovery callbacks with
    // IPCA_DEVICE_UPDATED_INFO status. Make a snapshot of all callbacks.
    std::vector<Callback::Ptr> callbackSnapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);
        callbackSnapshot = m_callbacks;
    }

    // Indicate discovery to apps.
    for (const auto& callback : callbackSnapshot)
    {
        callback->DeviceDiscoveryCallback(
                    true,
                    updatedDeviceInformation,
                    deviceDetails->deviceInfo,
                    deviceDetails->discoveredResourceTypes);
    }


    DebugOutputOCFDevices();

}

IPCAStatus OCFFramework::DiscoverAllResourcesGivenHost(std::string hostAddress)
{
    std::ostringstream resourceUri;
    OCConnectivityType connectivityType = CT_DEFAULT;

    // Request for all resources.
    resourceUri << OC_RSRVD_WELL_KNOWN_URI;
    OCStackResult result = OCPlatform::findResource(
                                            hostAddress,
                                            resourceUri.str(),
                                            connectivityType,
                                            std::bind(&OCFFramework::OnResourceFound, this, _1));

    if (result != OC_STACK_OK)
    {
        return IPCA_FAIL;
    }

    return IPCA_OK;
}

IPCAStatus OCFFramework::DiscoverResources(std::vector<std::string>& resourceTypeList)
{
    for (auto& resourceType : resourceTypeList)
    {
        std::ostringstream resourceUri;
        OCConnectivityType connectivityType = CT_DEFAULT;

        resourceUri << OC_RSRVD_WELL_KNOWN_URI;

        if (resourceType != "")
        {
            resourceUri << "?rt=" << resourceType;
        }

        OCStackResult result = OCPlatform::findResource(
                                    "",
                                    resourceUri.str(),
                                    connectivityType,
                                    std::bind(&OCFFramework::OnResourceFound, this, _1));

        if (result != OC_STACK_OK)
        {
            return IPCA_FAIL;
        }
    }

    return IPCA_OK;
}

void OCFFramework::OnDeviceInfoCallback(const OCRepresentation& rep)
{
    DeviceDetails::Ptr deviceDetails;

    {
        std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);

        if (m_OCFDevicesIndexedByDeviceURI.find(rep.getHost()) == m_OCFDevicesIndexedByDeviceURI.end())
        {
            OIC_LOG_V(WARNING, TAG,
                "OCFFramework::OnDeviceInfoCallback: Unknown device URI: [%s]",
                rep.getHost().c_str());
            return;
        }

        deviceDetails = m_OCFDevicesIndexedByDeviceURI[rep.getHost()];
        OCFFramework::DebugOutputOCRep(rep);

        if (deviceDetails == nullptr)
        {
            return; // there's no longer interest in this information.
        }

        if (deviceDetails->deviceInfoAvailable == true)
        {
            return;     // device info was processed before.
        }

        // Not reading "di" because it's already known in OnResourceFound().
        std::array<std::string, 3> keys = { {"n", "icv", "dmv"} };
        std::string dataModelVersion;
        std::vector<std::string*> Values =
        {
            &(deviceDetails->deviceInfo.deviceName),
            &(deviceDetails->deviceInfo.deviceSoftwareVersion),
            &dataModelVersion
        };

        for (size_t i = 0; i < keys.size(); i++)
        {
            rep.getValue(keys[i], *Values[i]);
        }

        // Add the device uri if it's new.
        if (std::find(deviceDetails->deviceUris.begin(),
                      deviceDetails->deviceUris.end(),
                      rep.getHost()) == deviceDetails->deviceUris.end())
        {
            deviceDetails->deviceUris.push_back(rep.getHost());
            m_OCFDevicesIndexedByDeviceURI[rep.getHost()] = deviceDetails;
        }

        deviceDetails->deviceInfo.deviceUris = deviceDetails->deviceUris;

        OCPlatform::getPropertyValue(
                        PAYLOAD_TYPE_DEVICE,
                        OC_RSRVD_DATA_MODEL_VERSION,
                        deviceDetails->deviceInfo.dataModelVersions);

        OCPlatform::getPropertyValue(
                        PAYLOAD_TYPE_DEVICE,
                        OC_RSRVD_PROTOCOL_INDEPENDENT_ID,
                        deviceDetails->deviceInfo.platformIndependentId);

        deviceDetails->deviceInfoAvailable = true;
    }

    // Inform apps.
    // Make a snapshot of all callbacks.
    std::vector<Callback::Ptr> callbackSnapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);
        callbackSnapshot = m_callbacks;
    }

    // Indicate discovery to apps.
    for (const auto& callback : callbackSnapshot)
    {
        callback->DeviceDiscoveryCallback(
                    true,   /* device is responding */
                    true,   /* this is an updated device info */
                    deviceDetails->deviceInfo,
                    deviceDetails->discoveredResourceTypes);
    }

    DebugOutputOCFDevices();
}

void OCFFramework::OnPlatformInfoCallback(const OCRepresentation& rep)
{
    std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);

    OCFFramework::DebugOutputOCRep(rep);

    if (m_OCFDevicesIndexedByDeviceURI.find(rep.getHost()) == m_OCFDevicesIndexedByDeviceURI.end())
    {
        OIC_LOG_V(WARNING, TAG, "OCFFramework::OnDeviceInfoCallback: Unknown device URI: [%s]",
            rep.getHost().c_str());
        return;
    }

    DeviceDetails::Ptr deviceDetails = m_OCFDevicesIndexedByDeviceURI[rep.getHost()];

    if (deviceDetails == nullptr)
    {
        return;     // there's no longer interest in this information.
    }

    if (deviceDetails->platformInfoAvailable == true)
    {
        return;     // multiple platform info received.
    }

    std::array<std::string, 11> keys =
        {
            {"pi", "mnmn", "mnml", "mnmo", "mndt", "mnpv", "mnos", "mnhw", "mnfv", "mnsl", "st"}
        };

    std::vector<std::string*> Values =
    {
        &(deviceDetails->platformInfo.platformId),
        &(deviceDetails->platformInfo.manufacturerName),
        &(deviceDetails->platformInfo.manufacturerURL),
        &(deviceDetails->platformInfo.modelNumber),
        &(deviceDetails->platformInfo.manufacturingDate),
        &(deviceDetails->platformInfo.platformVersion),
        &(deviceDetails->platformInfo.osVersion),
        &(deviceDetails->platformInfo.hardwareVersion),
        &(deviceDetails->platformInfo.firmwareVersion),
        &(deviceDetails->platformInfo.manufacturerSupportURL),
        &(deviceDetails->platformInfo.referenceTime)
    };

    for (size_t i = 0; i < keys.size(); i++)
    {
        rep.getValue(keys[i], *Values[i]);
    }

    deviceDetails->platformInfoAvailable = true;
    DebugOutputOCFDevices();
}

IPCAStatus OCFFramework::GetCommonResources(DeviceDetails::Ptr deviceDetails)
{
    const int MAX_REQUEST_COUNT = 3;

    OCStackResult result;

    // Get platform info if device hasn't responded to earlier request.
    if ((deviceDetails->platformInfoAvailable == false) &&
        (deviceDetails->platformInfoRequestCount < MAX_REQUEST_COUNT))
    {
        // Use host address of oic/p if the resource is returned by oic/res.
        std::string platformResourcePath(OC_RSRVD_PLATFORM_URI);
        std::string resourceType;
        std::shared_ptr<OCResource> platformResource = FindOCResource(deviceDetails,
                                                            platformResourcePath, resourceType);

        result = OCPlatform::getPlatformInfo(
                        platformResource ? platformResource->host() : deviceDetails->deviceUris[0],
                        OC_RSRVD_PLATFORM_URI,
                        CT_DEFAULT,
                        std::bind(&OCFFramework::OnPlatformInfoCallback, this, _1));

        if (result != OC_STACK_OK)
        {
            OIC_LOG_V(WARNING, TAG, "Failed getPlatformInfo() for: [%s] OC result: [%d]",
                deviceDetails->deviceUris[0], result);
        }

        deviceDetails->platformInfoRequestCount++;
    }

    // Get device info.
    if ((deviceDetails->deviceInfoAvailable == false) &&
        (deviceDetails->deviceInfoRequestCount < MAX_REQUEST_COUNT))
    {
        // Use host address of oic/d if the resource is returned by oic/res.
        std::string deviceResourcePath(OC_RSRVD_DEVICE_URI);
        std::string resourceType;
        std::shared_ptr<OCResource> deviceResource = FindOCResource(
                                                        deviceDetails,
                                                        deviceResourcePath,
                                                        resourceType);

        result = OCPlatform::getDeviceInfo(
                                    deviceResource ? deviceResource->host() :
                                                     deviceDetails->deviceUris[0],
                                    OC_RSRVD_DEVICE_URI,
                                    CT_DEFAULT,
                                    std::bind(&OCFFramework::OnDeviceInfoCallback, this, _1));
        if (result != OC_STACK_OK)
        {
            OIC_LOG_V(WARNING, TAG, "Failed getDeviceInfo() for [%s] OC result: [%d]",
                deviceDetails->deviceUris[0], result);
        }

        deviceDetails->deviceInfoRequestCount++;
    }

    // Get maintenance resource.
    if ((deviceDetails->maintenanceResourceAvailable == false) &&
        (deviceDetails->maintenanceResourceRequestCount < MAX_REQUEST_COUNT))
    {
        std::ostringstream deviceUri;
        OCConnectivityType connectivityType = CT_DEFAULT;
        deviceUri << OC_RSRVD_WELL_KNOWN_URI;
        deviceUri << "?rt=" << OC_RSRVD_RESOURCE_TYPE_MAINTENANCE;

        result = OCPlatform::findResource(
                                    deviceDetails->deviceUris[0],
                                    deviceUri.str(),
                                    connectivityType,
                                    std::bind(&OCFFramework::OnResourceFound, this, _1));

        if (result != OC_STACK_OK)
        {
            OIC_LOG_V(WARNING, TAG, "Failed findResource() for oic/mnt OC result: [%d]", result);
        }

        deviceDetails->maintenanceResourceRequestCount++;
    }

    return IPCA_OK;
}

IPCAStatus MapOCStackResultToIPCAStatus(OCStackResult result)
{
    IPCAStatus status;

    switch(result)
    {
        case OC_STACK_OK:
        case OC_STACK_CONTINUE:
        case OC_STACK_RESOURCE_CHANGED:
            status = IPCA_OK;
            break;

        case OC_STACK_UNAUTHORIZED_REQ:
            status = IPCA_ACCESS_DENIED;
            break;

        case OC_STACK_RESOURCE_CREATED:
            status = IPCA_RESOURCE_CREATED;
            break;

        case OC_STACK_RESOURCE_DELETED:
            status = IPCA_RESOURCE_DELETED;
            break;

        default:
            status = IPCA_FAIL;
            break;
    }

    return status;
}

// Callback handler on PUT request
void OCFFramework::OnPostPut(const HeaderOptions& headerOptions,
                        const OCRepresentation& rep,
                        const int eCode,
                        CallbackInfo::Ptr callbackInfo)
{
    OC_UNUSED(headerOptions);

    IPCAStatus status = MapOCStackResultToIPCAStatus((OCStackResult)eCode);

    for (const auto& callback : m_callbacks)
    {
        callback->SetCallback(status, rep, callbackInfo);
    }
}

// Callback handler on GET request
void OCFFramework::OnGet(const HeaderOptions& headerOptions,
                        const OCRepresentation& rep,
                        const int eCode,
                        CallbackInfo::Ptr callbackInfo)
{
    headerOptions;

    IPCAStatus status = IPCA_OK;

    if (eCode > OCStackResult::OC_STACK_RESOURCE_CHANGED)
    {
        status = IPCA_FAIL;
    }

    for (const auto& callback : m_callbacks)
    {
        callback->GetCallback(status, rep, callbackInfo);
    }
}

void OCFFramework::OnObserve(
                        const HeaderOptions headerOptions,
                        const OCRepresentation &rep,
                        const int &eCode,
                        const int &sequenceNumber,
                        CallbackInfo::Ptr callbackInfo)
{
    OC_UNUSED(sequenceNumber);

    IPCAStatus status = IPCA_OK;
    if (eCode > OCStackResult::OC_STACK_RESOURCE_CHANGED)
    {
        status = IPCA_FAIL;
    }

    for (const auto& callback : m_callbacks)
    {
        callback->ObserveCallback(status, rep, callbackInfo);
    }
}

void OCFFramework::OnDelete(const HeaderOptions& headerOptions,
                        const int eCode,
                        CallbackInfo::Ptr callbackInfo)
{
    OC_UNUSED(headerOptions);

    IPCAStatus status = MapOCStackResultToIPCAStatus((OCStackResult)eCode);

    for (const auto& callback : m_callbacks)
    {
        callback->DeleteResourceCallback(status, callbackInfo);
    }
}

IPCAStatus OCFFramework::SendCommandToDevice(std::string& deviceId,
                        CallbackInfo::Ptr callbackInfo,
                        OCRepresentation* rep)
{
    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return status;
    }

    std::shared_ptr<OCResource> ocResource = FindOCResource(
                                                deviceDetails,
                                                callbackInfo->resourcePath,
                                                callbackInfo->resourceType);
    if (ocResource == nullptr)
    {
        return IPCA_RESOURCE_NOT_FOUND;
    }

    QueryParamsMap queryParamsMap;
    if (callbackInfo->resourceType.empty() == false)
    {
        queryParamsMap[OC::Key::RESOURCETYPESKEY] = callbackInfo->resourceType;
    }

    if (callbackInfo->resourceInterface.empty() == false)
    {
        queryParamsMap[OC::Key::INTERFACESKEY] = callbackInfo->resourceInterface;
    }

    OCStackResult result = OC_STACK_ERROR;
    switch (callbackInfo->type)
    {
        case CallbackType_GetPropertiesComplete:
        {
            result = ocResource->get(
                        queryParamsMap,
                        std::bind(&OCFFramework::OnGet, this, _1, _2, _3, callbackInfo));
            break;
        }

        case CallbackType_SetPropertiesComplete:
        {
            result = ocResource->post(
                            *rep,
                            queryParamsMap,
                            std::bind(&OCFFramework::OnPostPut, this, _1, _2, _3, callbackInfo));
            break;
        }

        case CallbackType_CreateResourceComplete:
        {
            result = ocResource->post(
                            *rep,
                            queryParamsMap,
                            std::bind(&OCFFramework::OnPostPut, this, _1, _2, _3, callbackInfo));
            break;
        }

        case CallbackType_DeleteResourceComplete:
        {
            result = ocResource->deleteResource(
                            std::bind(&OCFFramework::OnDelete, this, _1, _2, callbackInfo));
            break;
        }

        case CallbackType_ResourceChange:
        {
            callbackInfo->ocResource = ocResource;
            result = ocResource->observe(
                                    ObserveType::Observe,
                                    queryParamsMap,
                                    std::bind(&OCFFramework::OnObserve, this,
                                            _1, _2, _3, _4, callbackInfo));
            break;
        }
    }

    if (result == OC_STACK_OK)
    {
        callbackInfo->requestSentTimestamp = OICGetCurrentTime(TIME_IN_MS);
        return IPCA_OK;
    }
    else
    {
        return IPCA_FAIL;
    }
}

void OCFFramework::StopObserve(CallbackInfo::Ptr cbInfo)
{
    std::shared_ptr<OCResource> ocResource = cbInfo->ocResource;
    ocResource->cancelObserve();
}

void OCFFramework::IsResourceObservable(std::string& deviceId,
                        const char* resourcePath,
                        bool* isObservable)
{
    *isObservable = false;

    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return;
    }

    if (deviceDetails->resourceMap.find(resourcePath) == deviceDetails->resourceMap.end())
    {
        return;
    }

    std::shared_ptr<OCResource> ocResource = deviceDetails->resourceMap[resourcePath];
    *isObservable = ocResource->isObservable();
}

IPCAStatus OCFFramework::PingDevice(std::string& deviceId)
{
    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return status;
    }

    std::ostringstream resourceUri;
    resourceUri << OC_RSRVD_WELL_KNOWN_URI;
    resourceUri << "?rt=" << OC_RSRVD_RESOURCE_TYPE_DEVICE;

    assert(deviceDetails->deviceUris.size() > 0);
    OCConnectivityType connectivityType = CT_DEFAULT;
    OCStackResult result = OCPlatform::findResource(
                                deviceDetails->deviceUris[0],
                                resourceUri.str(),
                                connectivityType,
                                std::bind(&OCFFramework::OnResourceFound, this, _1));

    if (result != OC_STACK_OK)
    {
        return IPCA_FAIL;
    }

    deviceDetails->lastPingTime = OICGetCurrentTime(TIME_IN_MS);
    return IPCA_OK;
}

IPCAStatus OCFFramework::GetLastPingTime(std::string& deviceId, uint64_t& lastPingTime)
{
    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return status;
    }

    lastPingTime = deviceDetails->lastPingTime;
    return IPCA_OK;
}

IPCAStatus OCFFramework::FindDeviceDetails(const std::string& deviceId,
                                           DeviceDetails::Ptr& deviceDetails)
{
    std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);

     auto device = m_OCFDevices.find(deviceId);
     if (device == m_OCFDevices.end())
     {
        return IPCA_FAIL;
     }

     deviceDetails = device->second;
     return IPCA_OK;
}

std::shared_ptr<OCResource> OCFFramework::FindOCResource(
                                                const DeviceDetails::Ptr& deviceDetails,
                                                const std::string& targetResourcePath,
                                                const std::string& targetRT)
{
    // Return resource matching resource path.
    if (deviceDetails->resourceMap.find(targetResourcePath) != deviceDetails->resourceMap.end())
    {
        return deviceDetails->resourceMap[targetResourcePath];
    }

    // No matching resource path. Return first resource that implements target resource type.
    for (auto const& resource : deviceDetails->resourceMap)
    {
        for (auto const& rt : resource.second->getResourceTypes())
        {
            if (rt.compare(targetRT) == 0)
            {
                return resource.second;
            }
        }
    }

    return nullptr;
}

IPCAStatus OCFFramework::CopyDeviceInfo(std::string& deviceId, IPCADeviceInfo** callerDeviceInfo)
{
    *callerDeviceInfo = nullptr;

    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return status;
    }

    // Determine if server has responded to OCPlatform::getDeviceInfo().
    if (deviceDetails->deviceInfoAvailable == false)
    {
        return IPCA_INFORMATION_NOT_AVAILABLE;
    }

    IPCADeviceInfo* deviceInfo = static_cast<IPCADeviceInfo*>(OICMalloc(sizeof(IPCADeviceInfo)));
    if (deviceInfo == nullptr)
    {
        return IPCA_OUT_OF_MEMORY;
    }

    memset(deviceInfo, 0, sizeof(IPCADeviceInfo));

    // @future: versionRequested determines what's copied to the caller buffer.
    deviceInfo->version = IPCA_VERSION_1;

    if (IPCA_OK != AllocateAndCopyStringVectorToArrayOfCharPointers(
                        deviceDetails->deviceUris,
                        const_cast<char***>(&deviceInfo->deviceUris),
                        &deviceInfo->deviceUriCount))
    {
        OICFree(deviceInfo);
        return IPCA_OUT_OF_MEMORY;
    }

    if (IPCA_OK != AllocateAndCopyStringVectorToArrayOfCharPointers(
                        deviceDetails->deviceInfo.dataModelVersions,
                        const_cast<char***>(&deviceInfo->dataModelVersions),
                        &deviceInfo->dataModelVersionCount))
    {
        FreeArrayOfCharPointers(const_cast<char**>(deviceInfo->deviceUris), deviceInfo->deviceUriCount);
        OICFree(deviceInfo);
        return IPCA_OUT_OF_MEMORY;
    }

    if ((IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->deviceId,
                        const_cast<char**>(&deviceInfo->deviceId))) ||
        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->deviceInfo.platformIndependentId,
                        const_cast<char**>(&deviceInfo->protocolIndependentId))) ||
        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->deviceInfo.deviceName,
                        const_cast<char**>(&deviceInfo->deviceName))) ||
        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->deviceInfo.deviceSoftwareVersion,
                        const_cast<char**>(&deviceInfo->deviceSoftwareVersion))))
    {
        FreeDeviceInfo(deviceInfo);
        return IPCA_OUT_OF_MEMORY;
    }

    *callerDeviceInfo = deviceInfo;
    return IPCA_OK;
}

void OCFFramework::FreeDeviceInfo(IPCADeviceInfo* deviceInfo)
{
    FreeArrayOfCharPointers(const_cast<char**>
                                (deviceInfo->deviceUris), deviceInfo->deviceUriCount);
    FreeArrayOfCharPointers(const_cast<char**>
                                (deviceInfo->dataModelVersions), deviceInfo->dataModelVersionCount);
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(deviceInfo->deviceId)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(deviceInfo->protocolIndependentId)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(deviceInfo->deviceName)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(deviceInfo->deviceSoftwareVersion)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(deviceInfo)));
}

IPCAStatus OCFFramework::CopyPlatformInfo(std::string& deviceId,
                                          IPCAPlatformInfo** callerPlatformInfo)
{
    *callerPlatformInfo = nullptr;

    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return status;
    }

    if (deviceDetails->platformInfoAvailable == false)
    {
        return IPCA_INFORMATION_NOT_AVAILABLE;
    }

    IPCAPlatformInfo* platformInfo = static_cast<IPCAPlatformInfo*>
                                        (OICMalloc(sizeof(IPCAPlatformInfo)));
    if (platformInfo == nullptr)
    {
        return IPCA_OUT_OF_MEMORY;
    }

    // @future: versionRequested determines what's copied to the caller buffer.
    platformInfo->version = IPCA_VERSION_1;

    if ((IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.platformId,
                        const_cast<char**>(&platformInfo->platformId)))                 ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.manufacturerName,
                        const_cast<char**>(&platformInfo->manufacturerName)))           ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.manufacturerURL,
                        const_cast<char**>(&platformInfo->manufacturerURL)))            ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.modelNumber,
                        const_cast<char**>(&platformInfo->modelNumber)))                ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.manufacturingDate,
                        const_cast<char**>(&platformInfo->manufacturingDate)))          ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.platformVersion,
                        const_cast<char**>(&platformInfo->platformVersion))             ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.osVersion,
                        const_cast<char**>(&platformInfo->osVersion)))                  ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.hardwareVersion,
                        const_cast<char**>(&platformInfo->hardwareVersion)))            ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.firmwareVersion,
                        const_cast<char**>(&platformInfo->firmwareVersion)))            ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.manufacturerSupportURL,
                        const_cast<char**>(&platformInfo->manufacturerSupportURL)))     ||

        (IPCA_OK != AllocateAndCopyStringToFlatBuffer(
                        deviceDetails->platformInfo.referenceTime,
                        const_cast<char**>(&platformInfo->referenceTime)))))
    {
        FreePlatformInfo(platformInfo);
        return IPCA_OUT_OF_MEMORY;
    }

    *callerPlatformInfo = platformInfo;
    return IPCA_OK;
}

void OCFFramework::FreePlatformInfo(IPCAPlatformInfo* platformInfo)
{
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->platformId)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->manufacturerName)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->manufacturerURL)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->modelNumber)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->manufacturingDate)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->platformVersion)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->osVersion)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->hardwareVersion)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->firmwareVersion)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->manufacturerSupportURL)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo->referenceTime)));
    OICFree(const_cast<void*>(reinterpret_cast<const void*>(platformInfo)));
}

IPCAStatus OCFFramework::CopyResourcePaths(const std::string& resourceInterface,
                                const std::string& resourceType,
                                std::string& deviceId,
                                std::vector<std::string>& resourcePathList)
{
    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return status;
    }

    for (auto const& resource : deviceDetails->resourceMap)
    {
        if ((!resourceInterface.empty()) &&
            (!IsStringInList(resourceInterface, resource.second->getResourceInterfaces())))
        {
            continue;
        }

        if ((!resourceType.empty()) &&
            (!IsStringInList(resourceType, resource.second->getResourceTypes())))
        {
            continue;
        }

        resourcePathList.push_back(resource.second->uri());
    }

    return IPCA_OK;
}

IPCAStatus OCFFramework::CopyResourceInfo(const std::string& deviceId,
                            const std::string& resourcePath,
                            ResourceInfoType resourceInfoType,
                            std::vector<std::string>& resourceInfo)
{
    DeviceDetails::Ptr deviceDetails;
    IPCAStatus status = FindDeviceDetails(deviceId, deviceDetails);
    if (status != IPCA_OK)
    {
        return status;
    }

    // Not specific resource.
    if (resourcePath.length() == 0)
    {
        switch(resourceInfoType)
        {
            case ResourceInfoType::ResourceType:
                resourceInfo = deviceDetails->discoveredResourceTypes;
                status = IPCA_OK;
                break;

            case ResourceInfoType::ResourceInterface:
                resourceInfo = deviceDetails->discoveredResourceInterfaces;
                status = IPCA_OK;
                break;

            default:
                status = IPCA_INVALID_ARGUMENT;
                break;
        }

        return status;
    }

    status = IPCA_RESOURCE_NOT_FOUND;

    // Filter for target resource URI.
    for (auto const& resource : deviceDetails->resourceMap)
    {
        if (resourcePath.compare(resource.second->uri()) == 0)
        {
            switch(resourceInfoType)
            {
                case ResourceInfoType::ResourceType:
                    resourceInfo = resource.second->getResourceTypes();
                    status = IPCA_OK;
                    break;

                case ResourceInfoType::ResourceInterface:
                    resourceInfo = resource.second->getResourceInterfaces();
                    status = IPCA_OK;
                    break;

                default:
                    status = IPCA_INVALID_ARGUMENT;
                    break;

            }
        }
    }

    return status;
}

void OCFFramework::DebugOutputOCFDevices()
{
#if DO_DEBUG
    std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);

    std::cout << "***** DebugOutputOCFDevices() ****" << std::endl;
    std::cout << "Device count: " << m_OCFDevices.size() << std::endl;

    // For each device
    for (auto const& device : m_OCFDevices)
    {
        std::cout << "Device URI    : " << device.first << std::endl;
        std::cout << "Device id     : " << device.second->deviceInfo.deviceId << std::endl;
        std::cout << "Device name   : " << device.second->deviceInfo.deviceName << std::endl;
        std::cout << "Resource Types: " << std::endl;
        for (auto const& res : device.second->discoveredResourceTypes)
        {
            std::cout << "   " << res.c_str() << std::endl;
        }

        // For each resource
        for (auto const& res : device.second->resourceMap)
        {
            std::cout << "Resource: " << res.first << std::endl;
            std::cout << "   URI: " << res.second->uri() << std::endl;

            // For each resource type
            std::vector<std::string> resourceTypes = res.second->getResourceTypes();
            for(auto rType : resourceTypes)
            {
                std::cout << "   Resource Type: " << rType.c_str() << std::endl;
            }
        }

        std::cout << std::endl;
    }
#endif
}

void OCFFramework::DebugOutputOCRep(const OCRepresentation& rep)
{
#if DO_DEBUG
    std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);
    PrintOCRep(rep);
#else
    OC_UNUSED(rep);
#endif
}

IPCAStatus OCFFramework::RequestAccess(std::string& deviceId,
                            CallbackInfo::Ptr callbackInfo,
                            CallbackInfo::Ptr passwordInputCallbackInfo)
{
    IPCAStatus status = IPCA_OK;
    DeviceDetails::Ptr deviceDetails;
    RequestAccessContext* requestAccessContext = nullptr;

    if (m_isStopping)
    {
        return IPCA_FAIL;
    }

    // Find the device details for this device
    status = FindDeviceDetails(deviceId, deviceDetails);
    if (status == IPCA_OK)
    {
        // Return a failure if an access request is already in progress for this device.
        if (!deviceDetails->securityInfo.isStarted)
        {
            deviceDetails->securityInfo.isStarted = true;
        }
        else
        {
            return IPCA_FAIL;
        }
    }
    else
    {
        return status;
    }

    // Construct context for the worker thread
    requestAccessContext = static_cast<RequestAccessContext*>
                                (OICCalloc(1, sizeof(RequestAccessContext)));
    if (nullptr != requestAccessContext)
    {
        requestAccessContext->deviceId = deviceId;
        requestAccessContext->ocfFramework = this;
        requestAccessContext->callbackInfo = callbackInfo;
        requestAccessContext->passwordInputCallbackInfo = passwordInputCallbackInfo;
    }
    else
    {
        return IPCA_OUT_OF_MEMORY;
    }

    // Add the context information to the list of contexts so we can clean it up later
    {
        std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);
        m_OCFRequestAccessContexts[deviceId] = requestAccessContext;
    }

    // Create a new thread to handle the RequestAccess request
    deviceDetails->securityInfo.requestAccessThread =
                std::thread(&OCFFramework::RequestAccessWorkerThread, requestAccessContext);

    return status;
}

void OCFFramework::RequestAccessWorkerThread(RequestAccessContext* requestContext)
{
    IPCAStatus status = IPCA_OK;
    IPCAStatus callbackStatus = IPCA_SECURITY_UPDATE_REQUEST_FAILED;
    OCStackResult result = OC_STACK_OK;
    std::string deviceId = requestContext->deviceId;
    OCFFramework* ocfFramework = requestContext->ocfFramework;
    CallbackInfo::Ptr callbackInfo = requestContext->callbackInfo;
    CallbackInfo::Ptr passwordInputCallbackInfo = requestContext->passwordInputCallbackInfo;
    DeviceDetails::Ptr deviceDetails;
    OicUuid_t uuid;

    // Check to make sure the OCFFramework is not shutting down before we start this request
    if (ocfFramework->m_isStopping)
    {
        status = IPCA_FAIL;
    }

    // Find the device details for this device and convert the device id into a UUID
    if (IPCA_OK == status)
    {
        status = ocfFramework->FindDeviceDetails(deviceId, deviceDetails);
        if (status == IPCA_OK)
        {
            result = ConvertStrToUuid(deviceId.c_str(), &uuid);
            if (OC_STACK_OK != result)
            {
                status = MapOCStackResultToIPCAStatus(result);
            }
        }
    }

    // Check to see if the device supports MOT
    if (IPCA_OK == status)
    {
        result = OCSecure::discoverMultipleOwnerEnabledDevice(
                    c_discoveryTimeout, &uuid, deviceDetails->securityInfo.device);

        if ((OC_STACK_OK == result) && (nullptr == deviceDetails->securityInfo.device))
        {
            status = IPCA_DEVICE_NOT_DISCOVERED;
        }
        else if(OC_STACK_OK != result)
        {
            status = MapOCStackResultToIPCAStatus(result);
        }
    }

    // Take ownership of the device if it supports MOT and the calling app is not a subowner.
    // Otherwise if the app is a subowner we will callback to the app indicating success without
    // doing anything.
    if ((IPCA_OK == status) && (nullptr != deviceDetails->securityInfo.device))
    {
        result = deviceDetails->securityInfo.device->isSubownerOfDevice(
                                            &deviceDetails->securityInfo.subowner);
        if (OC_STACK_OK == result)
        {
            deviceDetails->securityInfoAvailable = true;

            if (!deviceDetails->securityInfo.subowner)
            {
                // Check the selected ownership transfer method of the device to see if there
                // is anything we need to do before performing MOT
                switch (deviceDetails->securityInfo.device->getSelectedOwnershipTransferMethod())
                {
                    case OIC_RANDOM_DEVICE_PIN:
                    {
                        // Requests for a random pin will be handled by the underlying stack so
                        // there is nothing else to do
                        break;
                    }
                    case OIC_PRECONFIG_PIN:
                    {
                        char passwordBuffer[OXM_PRECONFIG_PIN_MAX_SIZE + 1];
                        size_t passwordBufferSize = OXM_PRECONFIG_PIN_MAX_SIZE + 1;
                        memset(passwordBuffer, 0, passwordBufferSize);

                        // We need to set the preconfigured pin before attempting to do MOT.
                        // Callback to the app asking for the password.
                        for (const auto& callback : ocfFramework->m_callbacks)
                        {
                            callback->PasswordInputCallback(deviceId,
                                        IPCA_OWNERSHIP_TRANSFER_PRECONFIGURED_PIN,
                                        passwordBuffer,
                                        passwordBufferSize,
                                        passwordInputCallbackInfo);
                        }

                        // Set the preconfigured pin
                        result = deviceDetails->securityInfo.device->addPreconfigPIN(
                                        passwordBuffer,
                                        strnlen_s(passwordBuffer, passwordBufferSize));

                        if (OC_STACK_OK != result)
                        {
                            status = MapOCStackResultToIPCAStatus(result);
                        }
                        break;
                    }
                    default:
                    {
                        // Preconfigured and random pin are the only supported MOT ownership
                        // transfer methods.
                        // Callback to the app reporting that the current selected method on the
                        // device is not supported and there needs to be intervention by the admin.
                        status = IPCA_FAIL;
                        callbackStatus = IPCA_SECURITY_UPDATE_REQUEST_NOT_SUPPORTED;
                        break;
                    }
                }

                if (IPCA_OK == status)
                {
                    std::unique_lock<std::mutex> lock(
                            deviceDetails->securityInfo.requestAccessThreadMutex);

                    result = deviceDetails->securityInfo.device->doMultipleOwnershipTransfer(
                                std::bind(
                                        &OCFFramework::OnMultipleOwnershipTransferCompleteCallback,
                                        ocfFramework,
                                        _1,
                                        _2,
                                        deviceId,
                                        callbackInfo));

                    if (OC_STACK_OK == result)
                    {
                        // Wait for the callback to indicate that MOT and calling back to the app
                        // has finished. If this takes longer
                        // then 30 seconds we assume that something has failed and continue.
                        // This is to prevent blocking forever and
                        // not allowing the app to close properly.
                        cv_status waitStatus =
                            deviceDetails->securityInfo.requestAccessThreadCV.wait_for(
                                                            lock, std::chrono::seconds(30));

                        if ((cv_status::timeout == waitStatus) || ocfFramework->m_isStopping)
                        {
                            status = IPCA_FAIL;
                        }
                    }
                    else
                    {
                        status = MapOCStackResultToIPCAStatus(result);
                    }
                }
            }
            else
            {
                // This app is already a subowner of the device
                for (const auto& callback : ocfFramework->m_callbacks)
                {
                    callback->RequestAccessCompletionCallback(
                                    IPCA_SECURITY_UPDATE_REQUEST_FINISHED,
                                    callbackInfo);
                }
            }
        }
        else
        {
            status = MapOCStackResultToIPCAStatus(result);
        }
    }

    // Callback to the application with the appropriate status information if we encountered an
    // issue while preparing to perform Multiple Ownership Transfer.
    // OnMultipleOwnershipTransferCompleteCallback will callback to the application to report the
    // success or failure of doMultipleOwnershipTransfer.
    if (IPCA_OK != status)
    {
        for (const auto& callback : ocfFramework->m_callbacks)
        {
            callback->RequestAccessCompletionCallback(callbackStatus, callbackInfo);
        }
    }
}

void OCFFramework::OnMultipleOwnershipTransferCompleteCallback(PMResultList_t* result,
                                    bool error,
                                    std::string deviceId,
                                    CallbackInfo::Ptr callbackInfo)
{
    OC_UNUSED(result);

    IPCAStatus status = IPCA_SECURITY_UPDATE_REQUEST_FINISHED;
    DeviceDetails::Ptr deviceDetails;

    // @todo: Provide more specific errors once the underlying IoTivity stack is able to provide us
    // with better error codes.
    if (error)
    {
        status = IPCA_SECURITY_UPDATE_REQUEST_FAILED;
    }

    for (const auto& callback : m_callbacks)
    {
        callback->RequestAccessCompletionCallback(status, callbackInfo);
    }

    // Get the device details so we can make sure the thread has finished and update the
    // request state accordingly
    status = FindDeviceDetails(deviceId, deviceDetails);
    if (status == IPCA_OK)
    {
        deviceDetails->securityInfo.subowner = true;
        deviceDetails->securityInfo.requestAccessThreadCV.notify_all();
    }
}

IPCAStatus OCFFramework::SetInputPasswordCallback(CallbackInfo::Ptr callbackInfo,
                                InputPinCallbackHandle* passwordInputCallbackHandle)
{
    OCSecure::registerInputPinCallback(std::bind(
                    &OCFFramework::OnPasswordInputCallback,
                    this,
                    _1,
                    _2,
                    _3,
                    callbackInfo),
                    passwordInputCallbackHandle);

    return IPCA_OK;
}

void OCFFramework::OnPasswordInputCallback(OicUuid_t deviceId,
                        char* passwordBuffer,
                        size_t passwordBufferSize,
                        CallbackInfo::Ptr callbackInfo)
{
    std::string strDeviceId;
    char uuidString[UUID_STRING_SIZE] = { 0 };
    OCConvertUuidToString(deviceId.id, uuidString);
    strDeviceId = uuidString;

    for (const auto& callback : m_callbacks)
    {
        callback->PasswordInputCallback(
                    strDeviceId,
                    IPCA_OWNERSHIP_TRANSFER_RANDOM_PIN,
                    passwordBuffer,
                    passwordBufferSize,
                    callbackInfo);
    }
}

IPCAStatus OCFFramework::SetDisplayPasswordCallback(CallbackInfo::Ptr callbackInfo,
                                DisplayPinCallbackHandle* passwordDisplayCallbackHandle)
{
    OCSecure::registerDisplayPinCallback(std::bind(
                        &OCFFramework::OnPasswordDisplayCallback,
                        this,
                        _1,
                        _2,
                        callbackInfo),
                        passwordDisplayCallbackHandle);

    return IPCA_OK;
}

void OCFFramework::OnPasswordDisplayCallback(char* passwordBuffer,
                            size_t passwordBufferSize,
                            CallbackInfo::Ptr callbackInfo)
{
    OC_UNUSED(passwordBufferSize);

    for (const auto& callback : m_callbacks)
    {
        callback->PasswordDisplayCallback("",
                    IPCA_OWNERSHIP_TRANSFER_RANDOM_PIN,
                    passwordBuffer,
                    callbackInfo);
    }
}

void OCFFramework::CleanupRequestAccessDevices()
{
    std::vector<DeviceDetails::Ptr> requestAccessDevices;

    // Discover all of the devices that performed security operations
    {
        std::lock_guard<std::recursive_mutex> lock(m_OCFFrameworkMutex);

        for (auto const& device : m_OCFDevices)
        {
            if (device.second->securityInfo.isStarted)
            {
                requestAccessDevices.push_back(device.second);
            }
        }
    }

    // If a RequestAccess operation is still in progress for a device wait for it to finish.
    // Once the operation is complete cleanup the RequestAccess context for the operation.
    for (auto const& device : requestAccessDevices)
    {
        device->securityInfo.requestAccessThreadCV.notify_all();

        if (device->securityInfo.requestAccessThread.joinable())
        {
            device->securityInfo.requestAccessThread.join();
        }

        auto context = m_OCFRequestAccessContexts.find(device->deviceId);
        if (context != m_OCFRequestAccessContexts.end())
        {
            RequestAccessContext* requestAccessContext = context->second;
            if (nullptr != requestAccessContext)
            {
                requestAccessContext->callbackInfo = nullptr;
                requestAccessContext->passwordInputCallbackInfo = nullptr;
                requestAccessContext->ocfFramework = nullptr;
                OICFree(static_cast<void*>(requestAccessContext));
            }
            m_OCFRequestAccessContexts.erase(device->deviceId);
        }
    }
}