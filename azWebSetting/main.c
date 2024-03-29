﻿/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

// This sample C application shows how to set up services on a private Ethernet network. It
// configures the network with a static IP address, starts the DHCP service allowing dynamically
// assigning IP address and network configuration parameters, enables the SNTP service allowing
// other devices to synchronize time via this device, and sets up a TCP server.
//
// It uses the API for the following Azure Sphere application libraries:
// - log (messages shown in Visual Studio's Device Output window during debugging)
// - networking (sets up private Ethernet configuration)

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

// applibs_versions.h defines the API struct versions to use for applibs APIs.
#include "applibs_versions.h"
#include "epoll_timerfd_utilities.h"

#include <applibs/log.h>
#include <applibs/networking.h>
#include <applibs/wificonfig.h>


#include "mt3620_avnet_dev.h"

#include "web_tcp_server.h"

// File descriptors - initialized to invalid value
 int epollFd = -1;
static int timerFd = -1;




// Termination state
static volatile sig_atomic_t terminationRequired = false;





/// <summary>
///     Signal handler for termination requests. This handler must be async-signal-safe.
/// </summary>
static void TerminationHandler(int signalNumber)
{
    // Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
    terminationRequired = true;
}


 

/// <summary>
///     Shut down TCP server and close epoll event handler.
/// </summary>
static void ShutDownServerAndCleanup(void)
{
    webServer_ShutDown(serverState);
    CloseFdAndPrintError(epollFd, "Epoll");
    CloseFdAndPrintError(timerFd, "Timer");
}

/// <summary>
///     Check network status and display information about all available network interfaces.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static int CheckNetworkStatus(void)
{
    // Ensure the necessary network interface is enabled.
    int result = Networking_SetInterfaceState(NetworkInterface, true);
    if (result != 0) {
        if (errno == EAGAIN) {
            Log_Debug("INFO: The networking stack isn't ready yet, will try again later.\n");
            return 0;
        } else {
            Log_Debug(
                "ERROR: Networking_SetInterfaceState for interface '%s' failed: errno=%d (%s)\n",
                NetworkInterface, errno, strerror(errno));
            return -1;
        }
    }
    isNetworkStackReady = true;

    // Display total number of network interfaces.
    ssize_t count = Networking_GetInterfaceCount();
    if (count == -1) {
        Log_Debug("ERROR: Networking_GetInterfaceCount: errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    Log_Debug("INFO: Networking_GetInterfaceCount: count=%zd\n", count);

    // Read current status of all interfaces.
    size_t bytesRequired = ((size_t)count) * sizeof(Networking_NetworkInterface);
    Networking_NetworkInterface *interfaces = malloc(bytesRequired);
    if (!interfaces) {
        abort();
    }

    ssize_t actualCount = Networking_GetInterfaces(interfaces, (size_t)count);
    if (actualCount == -1) {
        Log_Debug("ERROR: Networking_GetInterfaces: errno=%d (%s)\n", errno, strerror(errno));
    }
    Log_Debug("INFO: Networking_GetInterfaces: actualCount=%zd\n", actualCount);

    // Print detailed description of each interface.
    for (ssize_t i = 0; i < actualCount; ++i) {
        Log_Debug("INFO: interface #%zd\n", i);

        // Print the interface's name.
        char printName[IF_NAMESIZE + 1];
        memcpy(printName, interfaces[i].interfaceName, interfaces[i].interfaceNameLength);
        printName[interfaces[i].interfaceNameLength] = '\0';
        Log_Debug("INFO:   interfaceName=\"%s\"\n", interfaces[i].interfaceName);

        // Print whether the interface is enabled.
        Log_Debug("INFO:   isEnabled=\"%d\"\n", interfaces[i].isEnabled);

        // Print the interface's configuration type.
        Networking_IpType ipType = interfaces[i].ipConfigurationType;
        const char *typeText;
        switch (ipType) {
        case Networking_IpType_DhcpNone:
            typeText = "DhcpNone";
            break;
        case Networking_IpType_DhcpClient:
            typeText = "DhcpClient";
            break;
        default:
            typeText = "unknown-configuration-type";
            break;
        }
        Log_Debug("INFO:   ipConfigurationType=%d (%s)\n", ipType, typeText);

        // Print the interface's medium.
        Networking_InterfaceMedium_Type mediumType = interfaces[i].interfaceMediumType;
        const char *mediumText;
        switch (mediumType) {
        case Networking_InterfaceMedium_Unspecified:
            mediumText = "unspecified";
            break;
        case Networking_InterfaceMedium_Wifi:
            mediumText = "Wi-Fi";
            break;
        case Networking_InterfaceMedium_Ethernet:
            mediumText = "Ethernet";
            break;
        default:
            mediumText = "unknown-medium";
            break;
        }
        Log_Debug("INFO:   interfaceMediumType=%d (%s)\n", mediumType, mediumText);

        // Print the interface connection status
        Networking_InterfaceConnectionStatus status;
        int result = Networking_GetInterfaceConnectionStatus(interfaces[i].interfaceName, &status);
        if (result != 0) {
            Log_Debug("ERROR: Networking_GetInterfaceConnectionStatus: errno=%d (%s)\n", errno,
                      strerror(errno));
            return -1;
        }
        Log_Debug("INFO:   interfaceStatus=0x%02x\n", status);

		//If wifi no stored network, added a new OPEN wifi connect to SSID:"First"  
		if (WifiConfig_GetStoredNetworkCount()==0) {
			int newid = WifiConfig_AddNetwork();
			if (newid < 0) {
				Log_Debug("ERROR: Can not build new wifi network: errno=%d (%s)\n", errno,
					strerror(errno));
				return -1;
			}
			char* ssid = "First";
			WifiConfig_SetSSID(newid,ssid,strlen(ssid));
			WifiConfig_SetSecurityType(newid, WifiConfig_Security_Open);
			WifiConfig_SetNetworkEnabled(newid, true);
			WifiConfig_PersistConfig();
		}
    }

    

    return 0;
}

/// <summary>
///     Configure the specified network interface.
/// </summary>
/// <param name="interfaceName">
///     The name of the network interface to be configured.
/// </param>
/// <returns>0 on success, or -1 on failure</returns>
static int ConfigureNetworkInterface(const char *interfaceName)
{
    Networking_IpConfig ipConfig;
    Networking_IpConfig_Init(&ipConfig);
  //  inet_aton("192.168.100.10", &localServerIpAddress);
  //  inet_aton("255.255.255.0", &subnetMask);
  //  inet_aton("0.0.0.0", &gatewayIpAddress);
	
    Networking_IpConfig_EnableDynamicIp(&ipConfig);

    int result = Networking_IpConfig_Apply(interfaceName, &ipConfig);
    Networking_IpConfig_Destroy(&ipConfig);
    if (result != 0) {
        Log_Debug("ERROR: Networking_IpConfig_Apply: %d (%s)\n", errno, strerror(errno));
        return -1;
    }
    Log_Debug("INFO: Set dynamic IP address on network interface: %s.\n", interfaceName);

    return 0;
}

/// <summary>
///     Start SNTP server on the specified network interface.
/// </summary>
/// <param name="interfaceName">
///     The name of the network interface on which to start the SNTP server.
/// </param>
/// <returns>0 on success, or -1 on failure</returns>
static int StartSntpServer(const char *interfaceName)
{
    Networking_SntpServerConfig sntpServerConfig;
    Networking_SntpServerConfig_Init(&sntpServerConfig);
    int result = Networking_SntpServer_Start(interfaceName, &sntpServerConfig);
    Networking_SntpServerConfig_Destroy(&sntpServerConfig);
    if (result != 0) {
        Log_Debug("ERROR: Networking_SntpServer_Start: %d (%s)\n", errno, strerror(errno));
        return -1;
    }
    Log_Debug("INFO: SNTP server has started on network interface: %s.\n", interfaceName);
    return 0;
}

/// <summary>
///     Configure and start DHCP server on the specified network interface.
/// </summary>
/// <param name="interfaceName">
///     The name of the network interface on which to start the DHCP server.
/// </param>
/// <returns>0 on success, or -1 on failure</returns>
static int ConfigureAndStartDhcpSever(const char *interfaceName)
{
	

    Networking_DhcpServerConfig dhcpServerConfig;
    Networking_DhcpServerConfig_Init(&dhcpServerConfig);

    struct in_addr dhcpStartIpAddress;
    inet_aton("192.168.2.2", &dhcpStartIpAddress);

    Networking_DhcpServerConfig_SetLease(&dhcpServerConfig, dhcpStartIpAddress, 1, subnetMask,
                                         gatewayIpAddress, 24);
    Networking_DhcpServerConfig_SetNtpServerAddresses(&dhcpServerConfig, &localServerIpAddress, 1);

    int result = Networking_DhcpServer_Start(interfaceName, &dhcpServerConfig);
    Networking_DhcpServerConfig_Destroy(&dhcpServerConfig);
    if (result != 0) {
        Log_Debug("ERROR: Networking_DhcpServer_Start: %d (%s)\n", errno, strerror(errno));
        return -1;
    }
    Log_Debug("INFO: DHCP server has started on network interface: %s.\n", interfaceName);
    return 0;
}


static void MainServerStoppedHandler(webServer_StopReason reason) {
	ServerStoppedHandler(reason);
}


/// <summary>
///     Configure network interface, start SNTP server and TCP server.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static int CheckNetworkStackStatusAndLaunchServers(void)
{
    // Check the network stack readiness and display available interfaces when it's ready.
    if (CheckNetworkStatus() != 0) {
        return -1;
    }

    // The network stack is ready, so unregister the timer event handler and launch servers.
    if (isNetworkStackReady) {
        UnregisterEventHandlerFromEpoll(epollFd, timerFd);

   
        int result = ConfigureNetworkInterface(NetworkInterface);
        if (result != 0) {
            return -1;
        }

        // Start the SNTP server.
      //  result = StartSntpServer(NetworkInterface);
       // if (result != 0) {
       //     return -1;
       // }

   
        serverState = webServer_Start(epollFd, localServerIpAddress.s_addr, LocalTcpServerPort,
                                       serverBacklogSize, MainServerStoppedHandler);
        if (serverState == NULL) {
            return -1;
        }
    }

    return 0;
}



/// <summary>
///     The timer event handler.
/// </summary>
static void TimerEventHandler(EventData *eventData)
{
    if (ConsumeTimerFdEvent(timerFd) != 0) {
        terminationRequired = true;
        return;
    }

    // Check whether the network stack is ready.
    if (!isNetworkStackReady) {
        if (CheckNetworkStackStatusAndLaunchServers() != 0) {
            terminationRequired = true;
        }
    }
}

// event handler data structures. Only the event handler field needs to be populated.
static EventData afterPrcoessTimerEventData = {.eventHandler = &TimerEventHandler};

/// <summary>
///     Set up SIGTERM termination handler, set up epoll event handling, configure network
///     interface, start SNTP server and TCP server.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static int InitializeAndLaunchServers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = TerminationHandler;
    sigaction(SIGTERM, &action, NULL);

    epollFd = CreateEpollFd();
    if (epollFd < 0) {
        return -1;
    }

	
    // Check network interface status at the specified period until it is ready.
	// wlan should set longer check interval (around 8s) otherwise will hang
    struct timespec checkInterval = {10, 0};
    timerFd = CreateTimerFdAndAddToEpoll(epollFd, &checkInterval, &afterPrcoessTimerEventData, EPOLLIN);
    if (timerFd < 0) {
        return -1;
    }

    return 0;
}

/// <summary>
///     Main entry point for this application.
/// </summary>
int main(int argc, char *argv[])
{
    Log_Debug("INFO: Web setting server application starting.\n");
    if (InitializeAndLaunchServers() != 0) {
        terminationRequired = true;
    }

    // Use epoll to wait for events and trigger handlers, until an error or SIGTERM happens
    while (!terminationRequired) {
        if (WaitForEventAndCallHandler(epollFd) != 0) {
            terminationRequired = true;
        }
    }

    ShutDownServerAndCleanup();
    Log_Debug("INFO: Application exiting.\n");
    return 0;
}
