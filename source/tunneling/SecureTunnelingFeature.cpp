// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SecureTunnelingFeature.h"
#include "../logging/LoggerFactory.h"
#include "SecureTunnelingContext.h"
#include "TcpForward.h"
#include "EguanaTunneling.h"
#include <aws/crt/mqtt/MqttClient.h>
#include <aws/iotsecuretunneling/SubscribeToTunnelsNotifyRequest.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <map>
#include <memory>
#include <thread>

using namespace std;
using namespace Aws::Iotsecuretunneling;
using namespace Aws::Iot::DeviceClient::Logging;
using namespace Aws::Iot::DeviceClient::Util;

namespace Aws
{
    namespace Iot
    {
        namespace DeviceClient
        {
            namespace SecureTunneling
            {
                constexpr char SecureTunnelingFeature::TAG[];
                constexpr char SecureTunnelingFeature::NAME[];
                constexpr char SecureTunnelingFeature::DEFAULT_PROXY_ENDPOINT_HOST_FORMAT[];
                constexpr char SecureTunnelingFeature::TCP_OPERSTATE_FILE[];
                std::map<std::string, std::string> SecureTunnelingFeature::mServiceToAddressMap;
                std::map<std::string, uint16_t> SecureTunnelingFeature::mServiceToPortMap;

                SecureTunnelingFeature::SecureTunnelingFeature() = default;

                SecureTunnelingFeature::~SecureTunnelingFeature() = default;

                int SecureTunnelingFeature::init(
                    shared_ptr<SharedCrtResourceManager> sharedCrtResourceManager,
                    shared_ptr<ClientBaseNotifier> notifier,
                    const PlainConfig &config)
                {
                    sharedCrtResourceManager->initializeAWSHttpLib();

                    this->mSharedCrtResourceManager = sharedCrtResourceManager;
                    mClientBaseNotifier = notifier;

                    LoadFromConfig(config);

                    return 0;
                }

                string SecureTunnelingFeature::getName() { return NAME; }

                int SecureTunnelingFeature::start()
                {
                    RunSecureTunneling();
                    auto self = static_cast<Feature *>(this);
                    mClientBaseNotifier->onEvent(self, ClientBaseEventNotification::FEATURE_STARTED);
                    return 0;
                }

                int SecureTunnelingFeature::stop()
                {
                    LOG_DEBUG(TAG, "SecureTunnelingFeature::stop");
                    for (auto &c : mContexts)
                    {
                        c->StopSecureTunnel();
                    }

                    auto self = static_cast<Feature *>(this);
                    mClientBaseNotifier->onEvent(self, ClientBaseEventNotification::FEATURE_STOPPED);
                    return 0;
                }

                /**
                 * @brief Adds interlake endpoints to the serviceToAddressMap.
                 *
                 * This function is responsible for adding interlake endpoints to the provided serviceToAddressMap.
                 * The interlake endpoints are allocated based on the MAX_INTERLAKE_SYSTEM_SIZE constant.
                 * Each interlake system is assigned an IP address starting from "169.254.0.6" onwards.
                 * The master system is assigned the IP address "169.254.0.6", and subsequent slave systems are assigned IP addresses in sequential order.
                 *
                 * @param serviceToAddressMap The map to which the interlake endpoints will be added.
                 */
                void SecureTunnelingFeature::AddInterlakeEndpoints(std::map<std::string, std::string>& serviceToAddressMap)
                {
                    for (int i = 0; i < MAX_INTERLAKE_SYSTEM_SIZE; i++)
                    {
                        std::string key = TIVA_SERVICE_ID_PREFIX + std::to_string(i);
                        std::string value = TIVA_TCP_IP_ADDRESS_PREFIX + std::to_string(MASTER_SYSTEM_HOST_ID + i);
                        serviceToAddressMap[key] = value;
                    }
                }

                string SecureTunnelingFeature::GetAddressFromService(const std::string &service)
                {
                    if (mServiceToAddressMap.empty())
                    {
                        mServiceToAddressMap["SSH"] = EMC_NETWORK_BRIDGE_IP_ADDRESS;
                        mServiceToAddressMap["GW"] = EMC_NETWORK_BRIDGE_IP_ADDRESS;
                        mServiceToAddressMap["TIVA_TCP"] = TIVA_TCP_IP_ADDRESS;
                        mServiceToAddressMap["TIVA_RS485"] = TIVA_RS485_IP_ADDRESS;

                        AddInterlakeEndpoints(mServiceToAddressMap);
                    }

                    auto result = mServiceToAddressMap.find(AppendPostfixToService(service));
                    if (result == mServiceToAddressMap.end())
                    {
                        LOGM_ERROR(TAG, "Requested unsupported service. service=%s", service.c_str());
                        return ""; // TODO: Consider throw
                    }

                    return result->second;
                }

                uint16_t SecureTunnelingFeature::GetPortFromService(const std::string &service)
                {
                    if (mServiceToPortMap.empty())
                    {
                        mServiceToPortMap["SSH"] = SSH_TCP_PORT;
                        mServiceToPortMap["GW"] = GW_TCP_PORT;
                        mServiceToPortMap["TIVA"] = TIVA_TCP_PORT;
                    }

                    auto result = mServiceToPortMap.find(service);
                    if (result == mServiceToPortMap.end())
                    {
                        LOGM_ERROR(TAG, "Requested unsupported service. service=%s", service.c_str());
                        return 0; // TODO: Consider throw
                    }

                    return result->second;
                }

                string SecureTunnelingFeature::AppendPostfixToService(const string &service)
                {
                    if (service != "TIVA")
                    {
                        return service;
                    }

                    ifstream file(TCP_OPERSTATE_FILE);
                    if (file.is_open())
                    {
                        string state;
                        if (getline(file, state))
                        {
                            if (state == "up")
                            {
                                return service + "_TCP";
                            }
                        }
                    }

                    return service + "_RS485";
                }

                void SecureTunnelingFeature::StartDropbearServer()
                {
                    thread([]() {
                        system("/etc/init.d/dropbear start");
                    }).detach();

                    LOG_DEBUG(TAG, "Dropbear server is started");
                }

                void SecureTunnelingFeature::StartNetcatListener()
                {
                    int ret = system("pidof nc");
                    if (ret != 0)
                    {
                        LOG_DEBUG(TAG, "Starting netcat listener");
                        thread([]() {
                            system(("nc -l -p " + std::to_string(TIVA_TCP_PORT) + " > " + TIVA_RS485_DEVICE_FILE + " < " + TIVA_RS485_DEVICE_FILE).c_str());
                        }).detach();

                        /* Issue found during RS485 TIVA upgrade, looks like the cilent tries to talk to the destination before the netcat is effective.
                         * Added a one second delay to work around this for now.
                         */
                        sleep(1);

                        if (system("pidof nc") != 0)
                        {
                            LOG_ERROR(TAG, "Failed to start netcat listener");
                        }
                        else
                        {
                            LOG_DEBUG(TAG, "Netcat listener is started");
                        }
                    }
                    else
                    {
                        LOG_DEBUG(TAG, "Netcat listener is already running");
                    }
                }

                bool SecureTunnelingFeature::IsValidAddress(const string &address)
                {
                    struct sockaddr_in sa;
                    return inet_pton(AF_INET, address.c_str(), &(sa.sin_addr)) == 1;
                }

                bool SecureTunnelingFeature::IsValidPort(int port) { return 1 <= port && port <= 65535; }

                void SecureTunnelingFeature::LoadFromConfig(const PlainConfig &config)
                {
                    mThingName = *config.thingName;
                    mRootCa = config.rootCa;
                    mSubscribeNotification = config.tunneling.subscribeNotification;
                    mEndpoint = config.tunneling.endpoint;

                    if (!config.tunneling.subscribeNotification)
                    {
                        auto context = unique_ptr<SecureTunnelingContext>(new SecureTunnelingContext(
                            mSharedCrtResourceManager,
                            mRootCa,
                            *config.tunneling.destinationAccessToken,
                            GetEndpoint(*config.tunneling.region),
                            *config.tunneling.address,
                            static_cast<uint16_t>(config.tunneling.port.value()),
                            bind(&SecureTunnelingFeature::OnConnectionShutdown, this, placeholders::_1)));
                        mContexts.push_back(std::move(context));
                    }
                }

                void SecureTunnelingFeature::RunSecureTunneling()
                {
                    LOGM_INFO(TAG, "Running %s!", getName().c_str());

                    if (mSubscribeNotification)
                    {
                        SubscribeToTunnelsNotifyRequest request;
                        request.ThingName = mThingName.c_str();

                        iotSecureTunnelingClient = createClient();

                        iotSecureTunnelingClient->SubscribeToTunnelsNotify(
                            request,
                            AWS_MQTT_QOS_AT_LEAST_ONCE,
                            bind(
                                &SecureTunnelingFeature::OnSubscribeToTunnelsNotifyResponse,
                                this,
                                placeholders::_1,
                                placeholders::_2),
                            bind(&SecureTunnelingFeature::OnSubscribeComplete, this, placeholders::_1));
                    }
                    else
                    {
                        // Access token and region were loaded from config and have already been validated
                        for (auto &c : mContexts)
                        {
                            c->ConnectToSecureTunnel();
                        }
                    }
                }

                void SecureTunnelingFeature::OnSubscribeToTunnelsNotifyResponse(
                    SecureTunnelingNotifyResponse *response,
                    int ioErr)
                {
                    LOG_DEBUG(TAG, "Received MQTT Tunnel Notification");

                    if (ioErr || !response)
                    {
                        LOGM_ERROR(TAG, "OnSubscribeToTunnelsNotifyResponse received error. ioErr=%d", ioErr);
                        return;
                    }

                    for (auto &c : mContexts)
                    {
                        if (c->IsDuplicateNotification(*response))
                        {
                            LOG_INFO(TAG, "Received duplicate MQTT Tunnel Notification. Ignoring...");
                            return;
                        }
                    }

                    string clientMode = response->ClientMode->c_str();
                    if (clientMode != "destination")
                    {
                        LOGM_ERROR(TAG, "Unexpected client mode: %s", clientMode.c_str());
                        return;
                    }

                    if (!response->Services.has_value() || response->Services->empty())
                    {
                        LOG_ERROR(TAG, "no service requested");
                        return;
                    }
                    if (response->Services->size() > 1)
                    {
                        LOG_ERROR(
                            TAG,
                            "Received a multi-port tunnel request, but multi-port tunneling is not currently supported "
                            "by Device Client.");
                        return;
                    }

                    if (!response->ClientAccessToken.has_value() || response->ClientAccessToken->empty())
                    {
                        LOG_ERROR(TAG, "access token cannot be empty");
                        return;
                    }
                    string accessToken = response->ClientAccessToken->c_str();

                    if (!response->Region.has_value() || response->Region->empty())
                    {
                        LOG_ERROR(TAG, "region cannot be empty");
                        return;
                    }
                    string region = response->Region->c_str();

                    string service = response->Services->at(0).c_str();

                    string address = GetAddressFromService(service);

                    if (!IsValidAddress(address))
                    {
                        LOGM_ERROR(TAG, "Requested service %s is not supported: invalid destination IP address %s", service.c_str(), address.c_str());
                        return;
                    }

                    uint16_t port = GetPortFromService(service.substr(0, service.find("_")));

                    if (!IsValidPort(port))
                    {
                        LOGM_ERROR(TAG, "Requested service %s is not supported: invalid destination TCP port %u", service.c_str(), port);
                        return;
                    }

                    LOGM_DEBUG(TAG, "Region=%s, Service=%s, Destination=%s:%u", region.c_str(), service.c_str(), address.c_str(), port);

                    auto context = createContext(accessToken, region, address, port);

                    if (context->ConnectToSecureTunnel())
                    {
                        mContexts.push_back(std::move(context));
                    }
                }

                void SecureTunnelingFeature::OnSubscribeComplete(int ioErr) const
                {
                    LOG_DEBUG(TAG, "Subscribed to tunnel notification topic");

                    if (ioErr)
                    {
                        LOGM_ERROR(TAG, "Couldn't subscribe to tunnel notification topic. ioErr=%d", ioErr);
                        // TODO: Handle subscription error

                        // TODO: UA-5775 - Incorporate the baseClientNotifier onError event
                    }
                }

                string SecureTunnelingFeature::GetEndpoint(const string &region)
                {
                    if (mEndpoint.has_value())
                    {
                        return mEndpoint.value();
                    }

                    string endpoint = FormatMessage(DEFAULT_PROXY_ENDPOINT_HOST_FORMAT, region.c_str());

                    if (region.substr(0, 3) == "cn-")
                    {
                        // Chinese regions have ".cn" at the end:
                        // data.tunneling.iot.<region>.amazonaws.com.cn
                        // Examples of Chinese region name: "cn-north-1", "cn-northwest-1"
                        endpoint = endpoint + ".cn";
                    }

                    return endpoint;
                }

                std::unique_ptr<SecureTunnelingContext> SecureTunnelingFeature::createContext(
                    const std::string &accessToken,
                    const std::string &region,
                    const std::string &address,
                    const uint16_t &port)
                {
                    return std::unique_ptr<SecureTunnelingContext>(new SecureTunnelingContext(
                        mSharedCrtResourceManager,
                        mRootCa,
                        accessToken,
                        GetEndpoint(region),
                        address,
                        port,
                        bind(&SecureTunnelingFeature::OnConnectionShutdown, this, placeholders::_1)));
                }

                std::shared_ptr<AbstractIotSecureTunnelingClient> SecureTunnelingFeature::createClient()
                {
                    return std::make_shared<IotSecureTunnelingClientWrapper>(
                        mSharedCrtResourceManager->getConnection());
                }

                void SecureTunnelingFeature::OnConnectionShutdown(SecureTunnelingContext *contextToRemove)
                {
                    LOG_DEBUG(TAG, "SecureTunnelingFeature::OnConnectionShutdown");
                    auto it =
                        find_if(mContexts.begin(), mContexts.end(), [&](const unique_ptr<SecureTunnelingContext> &c) {
                            return c.get() == contextToRemove;
                        });
                    mContexts.erase(std::remove(mContexts.begin(), mContexts.end(), *it));

#if defined(DISABLE_MQTT)
                    LOG_INFO(TAG, "Secure Tunnel closed, component cleaning up open thread");
                    raise(SIGTERM);
#endif
                }

            } // namespace SecureTunneling
        }     // namespace DeviceClient
    }         // namespace Iot
} // namespace Aws
