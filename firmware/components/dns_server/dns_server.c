#include "dns_server.h"
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "DNS_SERVER";

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static int s_dns_socket = -1;
static TaskHandle_t s_dns_task_handle = NULL;
static volatile bool s_dns_running = false;

static int parse_dns_name(const uint8_t *buffer, int offset, int max_len, char *name_out, int name_max) {
    int name_len = 0;
    int curr = offset;
    
    while (curr < max_len) {
        uint8_t len = buffer[curr];
        if (len == 0) {
            curr++; // Skip terminating zero
            break;
        }
        
        // Compression pointer
        if ((len & 0xC0) == 0xC0) {
            return -1;
        }
        
        if (curr + 1 + len > max_len) {
            return -1;
        }
        
        if (name_len > 0 && name_len < name_max - 1) {
            name_out[name_len++] = '.';
        }
        
        for (int i = 0; i < len && name_len < name_max - 1; i++) {
            name_out[name_len++] = (char)buffer[curr + 1 + i];
        }
        
        curr += 1 + len;
    }
    
    if (name_len < name_max) {
        name_out[name_len] = '\0';
    } else {
        name_out[name_max - 1] = '\0';
    }
    
    return curr;
}

static const char* s_allowed_dns_domains[] = {
    "device.local",
    "www.device.local",
    "portal.local",
    "esp32.local",
    // Android Captive Probes
    "connectivitycheck.gstatic.com",
    "connectivitycheck.android.com",
    "clients3.google.com",
    // Apple iOS & macOS Captive Probes
    "captive.apple.com",
    "www.apple.com",
    "apple.com",
    // Microsoft Windows Captive Probes
    "www.msftconnecttest.com",
    "www.msftncsi.com",
    "msftconnecttest.com"
};

static void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[512];
    struct sockaddr_storage source_addr;
    socklen_t addr_len = sizeof(source_addr);
    
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);
    
    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket: errno %d", errno);
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    int opt = 1;
    setsockopt(s_dns_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    int err = bind(s_dns_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "DNS socket unable to bind: errno %d", errno);
        close(s_dns_socket);
        s_dns_socket = -1;
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS Captive Portal Server listening on port 53");
    s_dns_running = true;
    
    while (s_dns_running) {
        int len = recvfrom(s_dns_socket, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &addr_len);
        if (len < 0) {
            if (s_dns_running) {
                ESP_LOGD(TAG, "recvfrom error: %d", errno);
            }
            break;
        }
        
        if (len < sizeof(dns_header_t)) {
            continue;
        }
        
        dns_header_t *header = (dns_header_t *)rx_buffer;
        uint16_t qd_count = ntohs(header->qd_count);
        
        if (qd_count != 1) {
            continue;
        }
        
        char domain_name[128];
        int offset = parse_dns_name(rx_buffer, sizeof(dns_header_t), len, domain_name, sizeof(domain_name));
        if (offset < 0 || offset + 4 > len) {
            continue;
        }
        
        bool should_resolve = false;
        for (size_t i = 0; i < sizeof(s_allowed_dns_domains) / sizeof(s_allowed_dns_domains[0]); i++) {
            if (strcasecmp(domain_name, s_allowed_dns_domains[i]) == 0) {
                should_resolve = true;
                break;
            }
        }
        
        if (!should_resolve) {
            continue;
        }
        
        ESP_LOGD(TAG, "Resolving captive query: %s -> 192.168.4.1", domain_name);
        
        int q_len = offset + 4; // Header + question
        uint8_t tx_buffer[512];
        memcpy(tx_buffer, rx_buffer, q_len);
        
        dns_header_t *resp_header = (dns_header_t *)tx_buffer;
        resp_header->flags = htons(0x8580); // Standard authoritative answer
        resp_header->an_count = htons(1);
        resp_header->ns_count = 0;
        resp_header->ar_count = 0;
        
        uint8_t answer[] = {
            0xC0, 0x0C,            // Name pointer to question at offset 12
            0x00, 0x01,            // Type A (IPv4)
            0x00, 0x01,            // Class IN
            0x00, 0x00, 0x00, 0x3C, // TTL 60 seconds
            0x00, 0x04,            // RDLENGTH 4 bytes
            192, 168, 4, 1         // IP 192.168.4.1
        };
        
        if (q_len + sizeof(answer) <= sizeof(tx_buffer)) {
            memcpy(tx_buffer + q_len, answer, sizeof(answer));
            int resp_len = q_len + sizeof(answer);
            sendto(s_dns_socket, tx_buffer, resp_len, 0, (struct sockaddr *)&source_addr, addr_len);
        }
    }
    
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    ESP_LOGI(TAG, "DNS server task stopped");
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(void) {
    if (s_dns_task_handle != NULL) {
        ESP_LOGW(TAG, "DNS server is already running");
        return ESP_OK;
    }
    if (xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &s_dns_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DNS server task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void dns_server_stop(void) {
    if (s_dns_task_handle == NULL) {
        return;
    }
    s_dns_running = false;
    if (s_dns_socket >= 0) {
        shutdown(s_dns_socket, SHUT_RDWR);
        close(s_dns_socket);
        s_dns_socket = -1;
    }
}

bool dns_server_is_running(void) {
    return s_dns_running;
}
