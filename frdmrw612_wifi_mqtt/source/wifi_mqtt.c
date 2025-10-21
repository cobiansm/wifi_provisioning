/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2016-2022 NXP
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

#include "cred_flash_storage.h"
#include "lwip/api.h"
#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "wm_net.h"
#include "fsl_reset.h"
#include "webconfig.h"

#include "fsl_debug_console.h"
#include "mqtt_freertos.h"

#include <stdio.h>

#include "FreeRTOS.h"

/*******************************************************************************
 * Prototypes
 ******************************************************************************/

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

/*******************************************************************************
 * Variables
 ******************************************************************************/

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

/* Connect to the external AP */
static void ConnectTo()
{
    int32_t result;

    /* Add Wi-Fi network */
    result = WPL_AddNetwork(AP_SSID, AP_PASSWORD, WIFI_NETWORK_LABEL);
    if (result == WPLRET_SUCCESS)
    {
        PRINTF("Connecting as client to ssid: %s with password %s\r\n", AP_SSID, AP_PASSWORD);
        result = WPL_Join(WIFI_NETWORK_LABEL);
    }

    if (result != WPLRET_SUCCESS)
    {
        PRINTF("[!] Cannot connect to Wi-Fi\r\n[!]ssid: %s\r\n[!]passphrase: %s\r\n", AP_SSID, AP_PASSWORD);

        while (1)
        {
            __BKPT(0);
        }
    }
    else
    {
        PRINTF("[i] Connected to Wi-Fi\r\nssid: %s\r\n[!]passphrase: %s\r\n", AP_SSID, AP_PASSWORD);
        char ip[16];
        WPL_GetIP(ip, 1);
    }
}

/*!
 * @brief TCP server
 */
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
    char *pass = strtok(NULL, ",");

    if (ssid && pass) {
        PRINTF("SSID = %s | PASS = %s\r\n", ssid, pass);

        /* Save in Flash */
        save_wifi_credentials(CONNECTION_INFO_FILENAME, ssid, pass, "WPA2");
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
    uint32_t result = 0;

    PRINTF("\r\n************************************************\r\n");
    PRINTF(" Wi-Fi provisioning + MQTT client example\r\n");
    PRINTF("************************************************\r\n");

    init_flash_storage(CONNECTION_INFO_FILENAME);

	char ssid[WPL_WIFI_SSID_LENGTH];
	char password[WPL_WIFI_PASSWORD_LENGTH];
	char security[WIFI_SECURITY_LENGTH];

	result = get_saved_wifi_credentials(CONNECTION_INFO_FILENAME, ssid, password, security);

	if (result == 0 && strcmp(ssid, "") != 0)
	{
	   /* Credentials from last time have been found. The board will attempt to connect to this network as a client */
	   WC_DEBUG("[i] Saved SSID: %s, Password: %s, Security: %s\r\n", ssid, password, security);
	   g_BoardState.wifiState = WIFI_STATE_CLIENT;
	   strcpy(g_BoardState.ssid, ssid);
	   strcpy(g_BoardState.password, password);
	   strcpy(g_BoardState.security, security);
	}

    /* Check for credentials */
    if (init_flash_storage(CONNECTION_INFO_FILENAME) != 0) {
            PRINTF("[!] Flash init failed\n");
	}


	if (get_saved_wifi_credentials(CONNECTION_INFO_FILENAME, ssid, pass, security) == 0)
		PRINTF("[✔] Found saved credentials. Connecting to Wi-Fi...\r\n");

    /* Initialize Wi-Fi board */
    PRINTF("[i] Initializing Wi-Fi connection... \r\n");

    PRINTF("  before WPL_Init\r\n");
    result = WPL_Init();
    PRINTF("  after WPL_Init (result=%ld)\r\n", (long)result);
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

	PRINTF("[i] Successfully initialized Wi-Fi module\r\n");

	if (ssid[0] != '\0') {
		WPL_AddNetwork(ssid, pass, WIFI_NETWORK_LABEL);
		WPL_Join(WIFI_NETWORK_LABEL);

		/* Once connected, start MQTT client */
		mqtt_freertos_run_thread(netif_default);
		vTaskDelete(NULL);
	}

	PRINTF("[i] No credentials found. Starting SoftAP for provisioning.\r\n");

	/* Initialize Soft AP */
    PRINTF("Starting hands-on Wi-Fi Access Point\r\n");
    int err;
    err = WPL_Start_AP(AP_SSID, AP_PASSWORD, 1);
    if (err != WPLRET_SUCCESS)
    {
		PRINTF("[!] WPL_Start_AP: Failed, error: %d\r\n", (uint32_t)err);
		while (true);
    }
    else {
    	PRINTF("Wi-Fi AP interface up, DHCP server running.\r\n");
    }

    /* Start mDNS */
    LOCK_TCPIP_CORE();
    mdns_resp_init();
    mdns_resp_add_netif(net_get_uap_handle(), DEVICE_NAME);
    mdns_resp_add_service(net_get_uap_handle(), DEVICE_NAME, "_tcp",
    DNSSD_PROTO_TCP, TCP_PORT, NULL, NULL);
    UNLOCK_TCPIP_CORE();

    /* TCP Server */
    xTaskCreate(tcp_wait_for_credentials, "tcp_server", 2048, NULL, 3, NULL);

    //ConnectTo();
    /// wait_dns
    //mqtt_freertos_run_thread(netif_default);

    vTaskDelete(NULL);
}


/*!
 * @brief Main function.
 */
int main(void)
{
    /* Initialize the hardware */
    BOARD_InitHardware();

    /* Create the main Task */
    if (xTaskCreate(main_task, "main_task", 4096, NULL, configMAX_PRIORITIES - 4, NULL) != pdPASS)
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
