/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2016-2023 NXP
 * All rights reserved.
 *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*******************************************************************************
 * Includes
 ******************************************************************************/
#include "lwip/tcpip.h"
#include "board.h"
#include "app.h"
#include "wpl.h"
#include "timers.h"

#include "fsl_debug_console.h"
#include "webconfig.h"
#include "cred_flash_storage.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "lwip/api.h"
#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "wm_net.h"
#include "fsl_reset.h"

#include <stdio.h>

#include "FreeRTOS.h"

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static uint32_t SetBoardToClient();
static uint32_t SetBoardToAP();
static uint32_t CleanUpAP();
static uint32_t CleanUpClient();

/*******************************************************************************
 * Definitions
 ******************************************************************************/

#ifndef AP_SSID
#define AP_SSID "my_network"
#endif

#ifndef AP_PASSWORD
#define AP_PASSWORD "my_password"
#endif

#define TCP_PORT 10001
#define DEVICE_NAME "low_level_microcontroller"

typedef enum board_wifi_states
{
    WIFI_STATE_CLIENT,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CLIENT_SCAN,
    WIFI_STATE_AP,
    WIFI_STATE_AP_SCAN,
} board_wifi_states;

struct board_state_variables
{
    board_wifi_states wifiState;
    char ssid[WPL_WIFI_SSID_LENGTH];
    char password[WPL_WIFI_PASSWORD_LENGTH];
    char security[WIFI_SECURITY_LENGTH];
    bool connected;
    TaskHandle_t mainTask;
};

/*******************************************************************************
 * Variables
 ******************************************************************************/
struct board_state_variables g_BoardState;

/*******************************************************************************
 * Code
 ******************************************************************************/
/* Link lost callback */
static void LinkStatusChangeCallback(bool linkState)
{
    if (linkState == false)
    {
        /* -------- LINK LOST -------- */
        /* DO SOMETHING */
        PRINTF("-------- LINK LOST --------\r\n");
    }
    else
    {
        /* -------- LINK REESTABLISHED -------- */
        /* DO SOMETHING */
        PRINTF("-------- LINK REESTABLISHED --------\r\n");
    }
}

static void tcp_wait_for_credentials(void *arg)
{
    int server_fd, client_fd;
    struct sockaddr_in server, client;
    socklen_t client_len = sizeof(client);
    char buffer[128];

    /* Create TCP socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        PRINTF("[!] Error creating TCP socket\r\n");
        vTaskDelete(NULL);
    }

    /* Bind on port 10001 */
    server.sin_family = AF_INET;
    server.sin_port = PP_HTONS(TCP_PORT);
    server.sin_addr.s_addr = PP_HTONL(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        PRINTF("[!] Error in bind()\r\n");
        closesocket(server_fd);
        vTaskDelete(NULL);
    }

    listen(server_fd, 1);
    PRINTF("Waiting for credentials %d...\r\n", TCP_PORT);

    client_fd = accept(server_fd, (struct sockaddr *)&client, &client_len);
    PRINTF("[✔] Client connected\r\n");

    /* SSID & PASSWORD */
    int len = recv(client_fd, buffer, sizeof(buffer)-1, 0);
    buffer[len] = '\0';
    PRINTF("Received: %s\r\n", buffer);

    char *ssid = strtok(buffer, ",");
    char *password = strtok(NULL, ",");

    if (ssid && password) {
        PRINTF("SSID = %s | password = %s\r\n", ssid, password);

        /* Save in Flash */
        save_wifi_credentials(CONNECTION_INFO_FILENAME, ssid, password, "WPA2");
        send(client_fd, "Stored. Rebooting...\n", 26, 0);

        /* reset */
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        NVIC_SystemReset();
    } else {
        PRINTF("[!] Invalid format\r\n");
        send(client_fd, "ERROR\r\n", 7, 0);
    }

    closesocket(client_fd);
    closesocket(server_fd);
    vTaskDelete(NULL);
}

/*!
 * @brief The main task function
 */
static void main_task(void *arg)
{
    uint32_t result = 1;

    PRINTF(
        "\r\n"
        "Starting webconfig DEMO\r\n");

    /* When the App starts up, it will first read the mflash to check if any
     * credentials have been saved from previous runs.
     * If the mflash is empty, the board starts and AP allowing the user to configure
     * the desired Wi-Fi network.
     * Otherwise the stored credentials will be used to connect to the Wi-Fi network.*/
    WC_DEBUG("[i] Trying to load data from mflash.\r\n");

    init_flash_storage(CONNECTION_INFO_FILENAME);

    char ssid[WPL_WIFI_SSID_LENGTH];
    char password[WPL_WIFI_PASSWORD_LENGTH];
    char security[WIFI_SECURITY_LENGTH];

    result = get_saved_wifi_credentials(CONNECTION_INFO_FILENAME, ssid, password, security);

    if (result == 0 && strcmp(ssid, "") != 0)
    {
        /* Credentials from last time have been found. The board will attempt to
         * connect to this network as a client */
        WC_DEBUG("[i] Saved SSID: %s, password: %s, Security: %s\r\n", ssid, password, security);
        g_BoardState.wifiState = WIFI_STATE_CLIENT;
        strcpy(g_BoardState.ssid, ssid);
        strcpy(g_BoardState.password, password);
        strcpy(g_BoardState.security, security);
    }
    else
    {
        /* No credentials are stored, the board will start its own AP */
        WC_DEBUG("[i] Nothing stored yet\r\n");
        strcpy(g_BoardState.ssid, WIFI_SSID);
        strcpy(g_BoardState.password, WIFI_PASSWORD);
        g_BoardState.wifiState = WIFI_STATE_AP;
    }

    g_BoardState.connected = false;

    /* Initialize Wi-Fi board */
    WC_DEBUG("[i] Initializing Wi-Fi connection... \r\n");

    result = WPL_Init();
    if (result != WPLRET_SUCCESS)
    {
        PRINTF("[!] WPL Init failed: %d\r\n", (uint32_t)result);
        __BKPT(0);
    }

    result = WPL_Start(LinkStatusChangeCallback);
    if (result != WPLRET_SUCCESS)
    {
        PRINTF("[!] WPL Start failed %d\r\n", (uint32_t)result);
        __BKPT(0);
    }

    WC_DEBUG("[i] Successfully initialized Wi-Fi module\r\n");

    /* Start WebServer
    if (xTaskCreate(http_srv_task, "http_srv_task", HTTPD_STACKSIZE, NULL, HTTPD_PRIORITY, NULL) != pdpassword)
    {
        PRINTF("[!] HTTPD Task creation failed.");
        while (1)
            __BKPT(0);
    }*/

    /* Here other tasks can be created that will run the enduser app.... */

    /* Main Loop */
    while (1)
    {
        /* The SetBoardTo<state> function will configure the board Wifi to that given state.
         * After that, this task will suspend itself. It will remain suspended until it is time
         * to switch the state again. Uppon resuming, it will clean up the current state.
         * Every time the Wi-Fi state changes, this loop will perform an iteration switching back
         * and fourth between the two states as required.
         */
        switch (g_BoardState.wifiState)
        {
            case WIFI_STATE_CLIENT:
                SetBoardToClient();
                /* Suspend here until its time to swtich back to AP */
                vTaskSuspend(NULL);
                CleanUpClient();
                break;
            case WIFI_STATE_AP:
            default:
                SetBoardToAP();
                /* Suspend here until its time to stop the AP */
                vTaskSuspend(NULL);
                CleanUpAP();
        }
    }
}

static void flash_task(void *arg)
{
    uint32_t result = 0;

    char ssid[WPL_WIFI_SSID_LENGTH];
	char password[WPL_WIFI_PASSWORD_LENGTH];
	char security[WIFI_SECURITY_LENGTH];

    PRINTF("\r\n************************************************\r\n");
    PRINTF(" Wi-Fi provisioning + MQTT client example\r\n");
    PRINTF("************************************************\r\n");

    init_flash_storage(CONNECTION_INFO_FILENAME);

    if (get_saved_wifi_credentials(CONNECTION_INFO_FILENAME, ssid, password, security) == 0 && strcmp(ssid, "") != 0)
    {
		PRINTF("[✔] Found saved credentials. Connecting to Wi-Fi...\r\n");
		PRINTF("     SSID: %s | PASS: %s | SEC: %s\r\n", ssid, password, security);

		/* Initialize Wi-Fi board */
		result = WPL_Init();
		PRINTF("[i] Initializing Wi-Fi connection... \r\n");

		if (result != WPLRET_SUCCESS)
		{
		   PRINTF("[!] WPL Init failed: %d\r\n", (uint32_t)result);
		   __BKPT(0);
		}

		result = WPL_Start(LinkStatusChangeCallback);
		if (result != WPLRET_SUCCESS)
		{
		   PRINTF("[!] WPL Start failed %d\r\n", (uint32_t)result);
		   __BKPT(0);
		}

		PRINTF("[✔] Successfully initialized Wi-Fi module\r\n");

		WPL_AddNetwork(ssid, password, WIFI_NETWORK_LABEL);
		WPL_Join(WIFI_NETWORK_LABEL);

		 if (result == WPLRET_SUCCESS)
		 {
			PRINTF("[✔] Connected to Wi-Fi!\r\n");
			char ip[16];
			WPL_GetIP(ip, 1);
			PRINTF("[i] Device IP: %s\r\n", ip);
		 } else {
			PRINTF("[!] Failed to connect. Reset flash and try AP mode.\r\n");
		}
		/* Once connected, start MQTT client */
		//mqtt_freertos_run_thread(netif_default);
		vTaskDelete(NULL);
	}

	PRINTF("[i] No credentials found. Starting SoftAP for provisioning.\r\n");

	result = WPL_Init();
	if (result != WPLRET_SUCCESS) {
		PRINTF("[!] WPL_Init failed %d\r\n", result);
		__BKPT(0);
	}

	result = WPL_Start(LinkStatusChangeCallback);
	if (result != WPLRET_SUCCESS) {
		PRINTF("[!] WPL_Start failed %d\r\n", result);
		__BKPT(0);
	}

	PRINTF("[✔] Wi-Fi module initialized in AP mode\r\n");

	/* Initialize Soft AP */
    PRINTF("[i] Starting Wi-Fi Access Point\r\n");
    int err;
    err = WPL_Start_AP(AP_SSID, AP_PASSWORD, 1);
    if (err != WPLRET_SUCCESS)
    {
		PRINTF("[!] WPL_Start_AP: Failed, error: %d\r\n", (uint32_t)err);
		while (true);
    }
    else {
    	PRINTF("[i] Wi-Fi AP interface up, DHCP server running.\r\n");
    }

    PRINTF("[✔] AP up: SSID='%s', password='%s'\r\n", AP_SSID, AP_PASSWORD);

    char ap_ip[16];
    do {
        WPL_GetIP(ap_ip, 0);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    } while(strcmp(ap_ip, "0.0.0.0") == 0);

    PRINTF("[i] AP IP ready: %s\r\n", ap_ip);

    /* Start mDNS */
    LOCK_TCPIP_CORE();
    mdns_resp_init();
    mdns_resp_add_netif(net_get_uap_handle(), DEVICE_NAME);
    mdns_resp_add_service(net_get_uap_handle(), DEVICE_NAME, "_provision",
                          DNSSD_PROTO_TCP, TCP_PORT, NULL, NULL);
    UNLOCK_TCPIP_CORE();
    PRINTF("[mDNS] Service announced as %s._provision._tcp.local\r\n", DEVICE_NAME);

    /* TCP Server */
    xTaskCreate(tcp_wait_for_credentials, "tcp_server", 2048, NULL, 3, NULL);


    //ConnectTo();
    /// wait_dns
    //mqtt_freertos_run_thread(netif_default);

    //vTaskDelete(NULL);
}

/* Initialize and start local AP */
static uint32_t SetBoardToAP()
{
    uint32_t result;

    /* Set the global ssid and password to the default AP ssid and password */
    strcpy(g_BoardState.ssid, WIFI_SSID);
    strcpy(g_BoardState.password, WIFI_PASSWORD);

    /* Start the access point */
    PRINTF("Starting Access Point: SSID: %s, Chnl: %d\r\n", g_BoardState.ssid, WIFI_AP_CHANNEL);
    result = WPL_Start_AP(g_BoardState.ssid, g_BoardState.password, WIFI_AP_CHANNEL);

    if (result != WPLRET_SUCCESS)
    {
        PRINTF("[!] Failed to start access point\r\n");
        while (1)
            __BKPT(0);
    }
    g_BoardState.connected = true;

    char ip[16];
    WPL_GetIP(ip, 0);
    PRINTF(" Now join that network on your device and connect to this IP: %s\r\n", ip);

    return 0;
}

/* Clean up the local AP after waiting for all tasks to clean up */
static uint32_t CleanUpAP()
{
    /* Give time for reply message to reach the web interface before destorying the conection */
    vTaskDelay(10000 / portTICK_PERIOD_MS);

    WC_DEBUG("[i] Stopping AP!\r\n");
    if (WPL_Stop_AP() != WPLRET_SUCCESS)
    {
        PRINTF("Error while stopping ap\r\n");
        while (1)
            __BKPT(0);
    }

    return 0;
}

/* Connect to the external AP in g_BoardState.ssid */
static uint32_t SetBoardToClient()
{
    int32_t result;
    // If we are already connected, skip the initialization
    if (!g_BoardState.connected)
    {
        /* Add Wi-Fi network */
        if (strstr(g_BoardState.security, "WPA3_SAE"))
        {
            result = WPL_AddNetworkWithSecurity(g_BoardState.ssid, g_BoardState.password, WIFI_NETWORK_LABEL, WPL_SECURITY_WPA3_SAE);
        }
        else
        {
            result = WPL_AddNetworkWithSecurity(g_BoardState.ssid, g_BoardState.password, WIFI_NETWORK_LABEL, WPL_SECURITY_WILDCARD);
        }
        if (result == WPLRET_SUCCESS)
        {
            PRINTF("Connecting as client to ssid: %s with password %s\r\n", g_BoardState.ssid, g_BoardState.password);
            result = WPL_Join(WIFI_NETWORK_LABEL);
        }

        if (result != WPLRET_SUCCESS)
        {
            PRINTF("[!] Cannot connect to Wi-Fi\r\n[!]ssid: %s\r\n[!]passphrase: %s\r\n", g_BoardState.ssid,
                   g_BoardState.password);
            char c;
            do
            {
                PRINTF("[i] To reset the board to AP mode, press 'r'.\r\n");
                PRINTF("[i] In order to try connecting again press 'a'.\r\n");

                do
                {
                    c = GETCHAR();
                    // Skip over \n and \r and don't print the prompt again, just get next char
                } while (c == '\n' || c == '\r');

                switch (c)
                {
                    case 'r':
                    case 'R':
                        if (reset_saved_wifi_credentials(CONNECTION_INFO_FILENAME) != 0)
                        {
                            PRINTF("[!] Error occured during resetting of saved credentials!\r\n");
                            while (1)
                                __BKPT(0);
                        }
                        else
                        {
                            // Reset back to AP mode
                            g_BoardState.wifiState = WIFI_STATE_AP;
                            return 0;
                        }
                        break;
                    case 'a':
                    case 'A':
                        // Try connecting again...
                        return 0;
                    default:
                        PRINTF("Unknown command %c, please try again.\r\n", c);
                }

            } while (1);
        }
        else
        {
            PRINTF("[i] Connected to Wi-Fi\r\nssid: %s\r\n[!]passphrase: %s\r\n", g_BoardState.ssid,
                   g_BoardState.password);
            g_BoardState.connected = true;
            char ip[16];
            WPL_GetIP(ip, 1);
            PRINTF(" Now join that network on your device and connect to this IP: %s\r\n", ip);
        }
    }
    return 0;
}

/* Wait for any transmissions to finish and clean up the Client connection */
static uint32_t CleanUpClient()
{
    /* Give time for reply message to reach the web interface before destroying the connection */
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    /* Leave the external AP */
    if (WPL_Leave() != WPLRET_SUCCESS)
    {
        PRINTF("[!] Error Leaving from Client network.\r\n");
        __BKPT(0);
    }

    /* Remove the network profile */
    if (WPL_RemoveNetwork(WIFI_NETWORK_LABEL) != WPLRET_SUCCESS)
    {
        PRINTF("[!] Failed to remove network profile.\r\n");
        __BKPT(0);
    }

    return 0;
}
/*!
 * @brief Main function.
 */
int main(void)
{
    /* Initialize the hardware */
    BOARD_InitHardware();

    /* Create the main Task */
    if (xTaskCreate(flash_task, "main_task", 2048, NULL, configMAX_PRIORITIES - 4, &g_BoardState.mainTask) != pdPASS)
    {
        PRINTF("[!] MAIN Task creation failed!\r\n");
        while (1)
            ;
    }

    /* Run RTOS */
    vTaskStartScheduler();

    /* Should not reach this statement */
    for (;;)
        ;
}
