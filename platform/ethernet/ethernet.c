/* Ethernet Basic Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "ethernet.h"

#ifdef CONFIG_M_ROS_ESP_NETIF_ENET

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#if CONFIG_M_ROS_USE_SPI_ETHERNET
#include "driver/spi_master.h"
#endif // CONFIG_ETH_USE_SPI_ETHERNET

static EventGroupHandle_t s_ethernet_event_group;
static const char *TAG = "ethernet";

#if CONFIG_M_ROS_USE_SPI_ETHERNET
#define INIT_SPI_ETH_MODULE_CONFIG(eth_module_config, num)                                \
    do                                                                                    \
    {                                                                                     \
        eth_module_config[num].spi_cs_gpio = CONFIG_M_ROS_ETH_SPI_CS##num##_GPIO;         \
        eth_module_config[num].int_gpio = CONFIG_M_ROS_ETH_SPI_INT##num##_GPIO;           \
        eth_module_config[num].phy_reset_gpio = CONFIG_M_ROS_ETH_SPI_PHY_RST##num##_GPIO; \
        eth_module_config[num].phy_addr = CONFIG_M_ROS_ETH_SPI_PHY_ADDR##num;             \
    } while (0)

typedef struct
{
    uint8_t spi_cs_gpio;
    uint8_t int_gpio;
    int8_t phy_reset_gpio;
    uint8_t phy_addr;
} spi_eth_module_config_t;
#endif

/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id)
    {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        xEventGroupSetBits(s_ethernet_event_group, ETHERNET_FAIL_BIT);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        xEventGroupSetBits(s_ethernet_event_group, ETHERNET_FAIL_BIT);
        break;
    default:
        break;
    }
}

/* keep IP address obtained in event_handler */
static uint32_t mros2_ip_addr;
/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    /* keep IP address obtained in event_handler */
    mros2_ip_addr = event->ip_info.ip.addr;
    xEventGroupSetBits(s_ethernet_event_group, ETHERNET_CONNECTED_BIT);
}

#define	PIN_PHY_POWER GPIO_NUM_12
void init_network(void)
{
    s_ethernet_event_group = xEventGroupCreate();

    // Initialize TCP/IP network interface (should be called only once in application)
    ESP_ERROR_CHECK(esp_netif_init());
    // Create default event loop that running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_M_ROS_USE_INTERNAL_ETHERNET

    // Create new default instance of esp-netif for Ethernet
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);

    // Init MAC and PHY configs to default
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_mac_config_t eth_mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_mac_config.sw_reset_timeout_ms = 250; // Needed for some ethernet controllers to initiate

    phy_config.phy_addr = CONFIG_M_ROS_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_M_ROS_ETH_PHY_RST_GPIO;
    esp_rom_gpio_pad_select_gpio(PIN_PHY_POWER);
    gpio_set_direction(PIN_PHY_POWER,GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_PHY_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    emac_config.smi_mdc_gpio_num = 23;
    emac_config.smi_mdio_gpio_num = 18;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &eth_mac_config);

#if CONFIG_M_ROS_ETH_PHY_IP101
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
#elif CONFIG_M_ROS_ETH_PHY_RTL8201
    esp_eth_phy_t *phy = esp_eth_phy_new_rtl8201(&phy_config);
#elif CONFIG_M_ROS_ETH_PHY_LAN8720
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
#elif CONFIG_M_ROS_ETH_PHY_DP83848
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
#elif CONFIG_M_ROS_ETH_PHY_KSZ8041
    esp_eth_phy_t *phy = esp_eth_phy_new_ksz8041(&phy_config);
#elif CONFIG_M_ROS_ETH_PHY_KSZ8081
    esp_eth_phy_t *phy = esp_eth_phy_new_ksz8081(&phy_config);
#endif

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));
    /* attach Ethernet driver to TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
#endif // CONFIG_M_ROS_USE_INTERNAL_ETHERNET

#if CONFIG_M_ROS_USE_SPI_ETHERNET
    // Create instance(s) of esp-netif for SPI Ethernet(s)
    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t cfg_spi = {
        .base = &esp_netif_config,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    esp_netif_t *eth_netif_spi[CONFIG_M_ROS_SPI_ETHERNETS_NUM] = {NULL};
    char if_key_str[10];
    char if_desc_str[10];
    char num_str[3];
    for (int i = 0; i < CONFIG_M_ROS_SPI_ETHERNETS_NUM; i++)
    {
        itoa(i, num_str, 10);
        strcat(strcpy(if_key_str, "ETH_SPI_"), num_str);
        strcat(strcpy(if_desc_str, "eth"), num_str);
        esp_netif_config.if_key = if_key_str;
        esp_netif_config.if_desc = if_desc_str;
        esp_netif_config.route_prio = 30 - i;
        eth_netif_spi[i] = esp_netif_new(&cfg_spi);
    }

    // Init MAC and PHY configs to default
    eth_mac_config_t mac_config_spi = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config_spi = ETH_PHY_DEFAULT_CONFIG();

    // Install GPIO ISR handler to be able to service SPI Eth modlues interrupts
    gpio_install_isr_service(0);

    // Init SPI bus
    spi_device_handle_t spi_handle[CONFIG_M_ROS_SPI_ETHERNETS_NUM] = {NULL};
    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_M_ROS_ETH_SPI_MISO_GPIO,
        .mosi_io_num = CONFIG_M_ROS_ETH_SPI_MOSI_GPIO,
        .sclk_io_num = CONFIG_M_ROS_ETH_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_M_ROS_ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Init specific SPI Ethernet module configuration from Kconfig (CS GPIO, Interrupt GPIO, etc.)
    spi_eth_module_config_t spi_eth_module_config[CONFIG_M_ROS_SPI_ETHERNETS_NUM];
    INIT_SPI_ETH_MODULE_CONFIG(spi_eth_module_config, 0);
#if CONFIG_M_ROS_SPI_ETHERNETS_NUM > 1
    INIT_SPI_ETH_MODULE_CONFIG(spi_eth_module_config, 1);
#endif

    // Configure SPI interface and Ethernet driver for specific SPI module
    esp_eth_mac_t *mac_spi[CONFIG_M_ROS_SPI_ETHERNETS_NUM];
    esp_eth_phy_t *phy_spi[CONFIG_M_ROS_SPI_ETHERNETS_NUM];
    esp_eth_handle_t eth_handle_spi[CONFIG_M_ROS_SPI_ETHERNETS_NUM] = {NULL};
#if CONFIG_M_ROS_USE_KSZ8851SNL
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_M_ROS_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20};

    for (int i = 0; i < CONFIG_M_ROS_SPI_ETHERNETS_NUM; i++)
    {
        // Set SPI module Chip Select GPIO
        devcfg.spics_io_num = spi_eth_module_config[i].spi_cs_gpio;

        ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_M_ROS_ETH_SPI_HOST, &devcfg, &spi_handle[i]));
        // KSZ8851SNL ethernet driver is based on spi driver
        eth_ksz8851snl_config_t ksz8851snl_config = ETH_KSZ8851SNL_DEFAULT_CONFIG(spi_handle[i]);

        // Set remaining GPIO numbers and configuration used by the SPI module
        ksz8851snl_config.int_gpio_num = spi_eth_module_config[i].int_gpio;
        phy_config_spi.phy_addr = spi_eth_module_config[i].phy_addr;
        phy_config_spi.reset_gpio_num = spi_eth_module_config[i].phy_reset_gpio;

        mac_spi[i] = esp_eth_mac_new_ksz8851snl(&ksz8851snl_config, &mac_config_spi);
        phy_spi[i] = esp_eth_phy_new_ksz8851snl(&phy_config_spi);
    }
#elif CONFIG_M_ROS_USE_DM9051
    spi_device_interface_config_t devcfg = {
        .command_bits = 1,
        .address_bits = 7,
        .mode = 0,
        .clock_speed_hz = CONFIG_M_ROS_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20};

    for (int i = 0; i < CONFIG_M_ROS_SPI_ETHERNETS_NUM; i++)
    {
        // Set SPI module Chip Select GPIO
        devcfg.spics_io_num = spi_eth_module_config[i].spi_cs_gpio;

        ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_M_ROS_ETH_SPI_HOST, &devcfg, &spi_handle[i]));
        // dm9051 ethernet driver is based on spi driver
        eth_dm9051_config_t dm9051_config = ETH_DM9051_DEFAULT_CONFIG(spi_handle[i]);

        // Set remaining GPIO numbers and configuration used by the SPI module
        dm9051_config.int_gpio_num = spi_eth_module_config[i].int_gpio;
        phy_config_spi.phy_addr = spi_eth_module_config[i].phy_addr;
        phy_config_spi.reset_gpio_num = spi_eth_module_config[i].phy_reset_gpio;

        mac_spi[i] = esp_eth_mac_new_dm9051(&dm9051_config, &mac_config_spi);
        phy_spi[i] = esp_eth_phy_new_dm9051(&phy_config_spi);
    }
#elif CONFIG_M_ROS_USE_W5500
    spi_device_interface_config_t devcfg = {
        .command_bits = 16, // Actually it's the address phase in W5500 SPI frame
        .address_bits = 8,  // Actually it's the control phase in W5500 SPI frame
        .mode = 0,
        .clock_speed_hz = CONFIG_M_ROS_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20};

    for (int i = 0; i < CONFIG_M_ROS_SPI_ETHERNETS_NUM; i++)
    {
        // Set SPI module Chip Select GPIO
        devcfg.spics_io_num = spi_eth_module_config[i].spi_cs_gpio;

        ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_M_ROS_ETH_SPI_HOST, &devcfg, &spi_handle[i]));
        // w5500 ethernet driver is based on spi driver
        eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_handle[i]);

        // Set remaining GPIO numbers and configuration used by the SPI module
        w5500_config.int_gpio_num = spi_eth_module_config[i].int_gpio;
        phy_config_spi.phy_addr = spi_eth_module_config[i].phy_addr;
        phy_config_spi.reset_gpio_num = spi_eth_module_config[i].phy_reset_gpio;

        mac_spi[i] = esp_eth_mac_new_w5500(&w5500_config, &mac_config_spi);
        phy_spi[i] = esp_eth_phy_new_w5500(&phy_config_spi);
    }
#endif // CONFIG_M_ROS_USE_W5500

    for (int i = 0; i < CONFIG_M_ROS_SPI_ETHERNETS_NUM; i++)
    {
        esp_eth_config_t eth_config_spi = ETH_DEFAULT_CONFIG(mac_spi[i], phy_spi[i]);
        ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config_spi, &eth_handle_spi[i]));

        /* The SPI Ethernet module might not have a burned factory MAC address, we cat to set it manually.
       02:00:00 is a Locally Administered OUI range so should not be used except when testing on a LAN under your control.
        */
        ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle_spi[i], ETH_CMD_S_MAC_ADDR, (uint8_t[]){0x02, 0x00, 0x00, 0x12, 0x34, 0x56 + i}));

        // attach Ethernet driver to TCP/IP stack
        ESP_ERROR_CHECK(esp_netif_attach(eth_netif_spi[i], esp_eth_new_netif_glue(eth_handle_spi[i])));
    }
#endif // CONFIG_ETH_USE_SPI_ETHERNET

    // Register user defined event handers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

#if CONFIG_M_ROS_USE_INTERNAL_ETHERNET
    /* start Ethernet driver state machine */
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
#endif // CONFIG_M_ROS_USE_INTERNAL_ETHERNET
#if CONFIG_M_ROS_USE_SPI_ETHERNET
    for (int i = 0; i < CONFIG_M_ROS_SPI_ETHERNETS_NUM; i++)
    {
        ESP_ERROR_CHECK(esp_eth_start(eth_handle_spi[i]));
    }
#endif // CONFIG_M_ROS_USE_SPI_ETHERNET

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_ethernet_event_group,
                                           ETHERNET_CONNECTED_BIT | ETHERNET_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & ETHERNET_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to network via ethernet.");
    }
    else if (bits & ETHERNET_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Connection to network via ethernet failed.");
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

uint32_t get_mros2_ip_addr(void)
{
    return mros2_ip_addr;
}

#endif // CONFIG_M_ROS_ESP_NETIF_ENET