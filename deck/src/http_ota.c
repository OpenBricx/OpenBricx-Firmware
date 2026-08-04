// OpenBricx Deck — HTTP OTA endpoint. `POST /obx/ota` with the app image
// (firmware.bin) as the body; the handler streams it into the spare OTA slot via
// esp_ota, validates, sets it as the boot partition, and reboots.

#include "http_ota.h"

#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_ota";
static httpd_handle_t s_server = NULL;

#define OTA_RECV_BUF 1460  // ~1 TCP segment

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = 0;
    // Sequential writes erase lazily per sector, so there's no long blocking erase
    // up front (which would also stall the HTTP socket).
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }

    char buf[OTA_RECV_BUF];
    int remaining = req->content_len;
    int written = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(handle, buf, n) != ESP_OK) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
            return ESP_FAIL;
        }
        written += n;
        remaining -= n;
    }

    if (esp_ota_end(handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image invalid");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA complete (%d bytes) -> %s; rebooting", written, part->label);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));  // let the response flush
    esp_restart();
    return ESP_OK;
}

void http_ota_start(void)
{
    if (s_server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return;
    }
    httpd_uri_t ota_uri = {
        .uri = "/obx/ota",
        .method = HTTP_POST,
        .handler = ota_post,
    };
    httpd_register_uri_handler(s_server, &ota_uri);
    ESP_LOGI(TAG, "OTA endpoint ready: POST /obx/ota");
}

void http_ota_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
}
