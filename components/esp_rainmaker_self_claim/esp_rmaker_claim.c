// Copyright 2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include "mbedtls/config.h"
#include "mbedtls/platform.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/md.h"
#include "mbedtls/sha512.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_event.h>
#include <esp_tls.h>
#include <string.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <json_generator.h>
#include <json_parser.h>

#include "esp_rmaker_claim.h"

static const char *TAG = "esp_claim";

#define ESP_RMAKER_RANDOM_NUMBER_LEN    64

#include "soc/soc.h"
#include "soc/efuse_reg.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"

#define CLAIM_BASE_URL      "https://esp-claiming.rainmaker.espressif.com"
#define CLAIM_INIT_PATH     "claim/initiate"
#define CLAIM_VERIFY_PATH   "claim/verify"

extern uint8_t claim_service_server_root_ca_pem_start[] asm("_binary_rmaker_claim_service_server_crt_start");
extern uint8_t claim_service_server_root_ca_pem_end[] asm("_binary_rmaker_claim_service_server_crt_end");

#define CLAIM_PK_SIZE       2048

static EventGroupHandle_t claim_event_group;
static const int CLAIM_TASK_BIT = BIT0;

static char *self_claim_certificate = NULL;
static char *self_claim_private_key = NULL;
static const char *self_claim_name = NULL;

static void escape_new_line(esp_rmaker_claim_data_t *data)
{
    char *str = (char *)data->csr;
    memset(data->payload, 0, sizeof(data->payload));
    char *target_str = (char *)data->payload;
    /* Hack to just avoid a "\r\n" at the end of string */
    if (str[strlen(str) - 1] == '\n') {
        str[strlen(str) - 1] = '\0';
    }
    while (*str) {
        if (*str == '\n') {
            *target_str++ = '\\';
            *target_str++ = 'n';
            str++;
            continue;
        }
        *target_str++ = *str++;
    }
    *target_str = '\0';
    strcpy((char *)data->csr, (char *)data->payload);
    ESP_LOGD(TAG, "Modified CSR : %s", data->csr);
}

static void unescape_new_line(char *str)
{
    char *target_str = str;
    while (*str) {
        if (*str == '\\') {
            str++;
            if (*str == 'n') {
                *target_str++ = '\n';
                str++;
            }
        }
        *target_str++ = *str++;
    }
    *target_str = '\0';
}

static esp_err_t esp_rmaker_claim_generate_csr(esp_rmaker_claim_data_t *claim_data, const char *common_name)
{
    if (!claim_data || !common_name) {
        ESP_LOGE(TAG, "claim_data or common_name cannot be NULL.");
        return ESP_ERR_INVALID_ARG;
    }
    const char *pers = "gen_csr";
    mbedtls_x509write_csr csr;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;

    /* Generating CSR from the private key */
    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    ESP_LOGD(TAG, "Seeding the random number generator.");
    mbedtls_entropy_init(&entropy);
    int ret = mbedtls_ctr_drbg_seed( &ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *) pers, strlen(pers));
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed returned -0x%04x", -ret );
        goto exit;
    }
    char subject_name[50];
    snprintf(subject_name, sizeof(subject_name), "CN=%s", common_name);
    ret = mbedtls_x509write_csr_set_subject_name(&csr, subject_name);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_x509write_csr_set_subject_name returned %d", ret );
        goto exit;
    }

    memset(claim_data->csr, 0, sizeof(claim_data->csr));
    mbedtls_x509write_csr_set_key(&csr, &claim_data->key);
    ESP_LOGD(TAG, "Generating PEM");
    ret = mbedtls_x509write_csr_pem(&csr, claim_data->csr, sizeof(claim_data->csr), mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret < 0) {
        ESP_LOGE(TAG, "mbedtls_x509write_csr_pem returned -0x%04x", -ret );
        goto exit;
    }
    ESP_LOGD(TAG, "CSR generated.");
    claim_data->state = RMAKER_CLAIM_STATE_CSR_GENERATED;
exit:

    mbedtls_x509write_csr_free(&csr);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return ret;
}

static esp_err_t esp_rmaker_claim_generate_key(esp_rmaker_claim_data_t *claim_data)
{
    const char *pers = "gen_key";
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    mbedtls_pk_free(&claim_data->key);
    mbedtls_pk_init(&claim_data->key);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    memset(claim_data->payload, 0, sizeof(claim_data->payload));

    ESP_LOGD(TAG, "Seeding the random number generator.");
    mbedtls_entropy_init(&entropy);
    int ret = mbedtls_ctr_drbg_seed( &ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *) pers, strlen(pers));
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed returned -0x%04x", -ret );
        mbedtls_pk_free(&claim_data->key);
        goto exit;
    }

    ESP_LOGW(TAG, "Generating the private key. This may take time." );
    ret = mbedtls_pk_setup(&claim_data->key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_setup returned -0x%04x", -ret );
        mbedtls_pk_free(&claim_data->key);
        goto exit;
    }

    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(claim_data->key), mbedtls_ctr_drbg_random, &ctr_drbg, CLAIM_PK_SIZE, 65537); /* here, 65537 is the RSA exponent */
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_rsa_gen_key returned -0x%04x", -ret );
        mbedtls_pk_free(&claim_data->key);
        goto exit;
    }

    claim_data->state = RMAKER_CLAIM_STATE_PK_GENERATED;
    ESP_LOGD(TAG, "Converting Private Key to PEM...");
    ret = mbedtls_pk_write_key_pem(&claim_data->key, (unsigned char *)claim_data->payload, sizeof(claim_data->payload));
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_write_key_pem returned -0x%04x", -ret );
        mbedtls_pk_free(&claim_data->key);
    }
exit:
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

/* Parse the Claim Init response and generate Claim Verify request
 *
 * Claim Verify Response format:
 *  {"certificate":"<certificate>"}
 */
static esp_err_t handle_claim_verify_response(esp_rmaker_claim_data_t *claim_data)
{
    ESP_LOGD(TAG, "Claim Verify Response: %s", claim_data->payload);
    jparse_ctx_t jctx;
    if (json_parse_start(&jctx, claim_data->payload, strlen(claim_data->payload)) == 0) {
        int required_len = 0;
        if (json_obj_get_strlen(&jctx, "certificate", &required_len) == 0) {
            required_len++; /* For NULL termination */
            self_claim_certificate =  calloc(1, required_len + 1);
            if (!self_claim_certificate) {
                json_parse_end(&jctx);
                ESP_LOGE(TAG, "Failed to allocate %d bytes for certificate.", required_len);
                return ESP_ERR_NO_MEM;
            }
            json_obj_get_string(&jctx, "certificate", self_claim_certificate, required_len);
            json_parse_end(&jctx);
            unescape_new_line(self_claim_certificate);
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Claim Verify Response invalid.");
        }
    }
    ESP_LOGE(TAG, "Failed to parse Claim Verify Response.");
    return ESP_FAIL;
}

static esp_err_t generate_claim_init_request(esp_rmaker_claim_data_t *claim_data)
{
    if (claim_data->state < RMAKER_CLAIM_STATE_PK_GENERATED) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t eth_mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not fetch MAC address. Please initialise Wi-Fi first");
        return err;
    }

    snprintf(claim_data->payload, sizeof(claim_data->payload),
            "{\"mac_addr\":\"%02X%02X%02X%02X%02X%02X\",\"platform\":\"%s\"}",
            eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5], CONFIG_IDF_TARGET);
    claim_data->payload_len = strlen(claim_data->payload);
    claim_data->payload_offset = 0;
    return ESP_OK;
}

void esp_rmaker_claim_data_free(esp_rmaker_claim_data_t *claim_data)
{
    if(claim_data) {
        mbedtls_pk_free(&claim_data->key);
        free(claim_data);
    }
}

static esp_err_t read_hmac_key(uint32_t *out_hmac_key, size_t hmac_key_size)
{
    /* ESP32-S2 HMAC Key programming scheme */
    if (hmac_key_size != 16) {
        ESP_LOGE(TAG, "HMAC key size should be 16 bytes.");
    }
    esp_err_t err = esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, out_hmac_key, hmac_key_size * 8);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_efuse_read_field_blob failed!");
    }
    return err;
}

static esp_err_t hmac_challenge(const char* hmac_request, unsigned char *hmac_response, size_t len_hmac_response)
{
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA512;
    uint32_t hmac_key[4];
    
    esp_err_t err = read_hmac_key(hmac_key, sizeof(hmac_key));
    if (err != ESP_OK) {
        return err;
    }

    mbedtls_md_init(&ctx);  
    int ret = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type) ,1); 
    ret |= mbedtls_md_hmac_starts(&ctx, (const unsigned char *)hmac_key, sizeof(hmac_key));
    ret |= mbedtls_md_hmac_update(&ctx, (const unsigned char *)hmac_request, strlen(hmac_request));    
    ret |= mbedtls_md_hmac_finish(&ctx, hmac_response);   
    mbedtls_md_free(&ctx);

    if(ret == 0) {
        return ESP_OK;
    } else {
        return ret;
    }
}

/* Parse the Claim Init response and generate Claim Verify request
 *
 * Claim Init Response format:
 *  {"auth_id":"<unique-auth-id>", "challenge":"<upto 128 byte challenge>"}
 *
 * Claim Verify Request format
 *  {"auth_id":"<claim-id-from-init>", "challenge_response":"<64byte-response-in-hex>, "csr":"<csr-generated-earlier>"}
 */
static esp_err_t handle_self_claim_init_response(esp_rmaker_claim_data_t *claim_data)
{
    ESP_LOGD(TAG, "Claim Init Response: %s", claim_data->payload);
    jparse_ctx_t jctx;
    if (json_parse_start(&jctx, claim_data->payload, strlen(claim_data->payload)) == 0) {
        char auth_id[64];
        char challenge[130];
        int ret = json_obj_get_string(&jctx, "auth_id", auth_id, sizeof(auth_id));
        ret |= json_obj_get_string(&jctx, "challenge", challenge, sizeof(challenge));
        json_parse_end(&jctx);
        if (ret == 0) {
            unsigned char response[64] = {0};
            if (hmac_challenge(challenge, response, sizeof(response)) == ESP_OK) {
                json_gen_str_t jstr;
                json_gen_str_start(&jstr, claim_data->payload, sizeof(claim_data->payload), NULL, NULL);
                json_gen_start_object(&jstr);
                json_gen_obj_set_string(&jstr, "auth_id", auth_id);
                /* Add Challenge Response as a hex representation */
                json_gen_obj_start_long_string(&jstr, "challenge_response", NULL);
                for(int i = 0 ; i < sizeof(response); i++) {
                    char hexstr[3];
                    snprintf(hexstr, sizeof(hexstr), "%02X", response[i]);
                    json_gen_add_to_long_string(&jstr, hexstr);
                }
                json_gen_end_long_string(&jstr);
                json_gen_obj_set_string(&jstr, "csr", (char *)claim_data->csr);
                json_gen_end_object(&jstr);
                json_gen_str_end(&jstr);
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "HMAC Challenge failed.");
            }
        } else {
            ESP_LOGE(TAG, "Claim Init Response invalid.");
        }
    }
    ESP_LOGE(TAG, "Failed to parse Claim Init Response.");
    return ESP_FAIL;
}
static esp_err_t esp_rmaker_claim_perform_common(esp_rmaker_claim_data_t *claim_data, const char *path)
{
    char url[100];
    snprintf(url, sizeof(url), "%s/%s", CLAIM_BASE_URL, path);
    esp_http_client_config_t config = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .buffer_size = 1024,
        .cert_pem = (const char *)claim_service_server_root_ca_pem_start,
        .skip_cert_common_name_check = false
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialise HTTP Client.");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Payload for %s: %s", url, claim_data->payload);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_err_t err = esp_http_client_open(client, strlen(claim_data->payload));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open connection to %s", url);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    int len = esp_http_client_write(client, claim_data->payload, strlen(claim_data->payload));
    if (len != strlen(claim_data->payload)) {
        ESP_LOGE(TAG, "Failed to write Payload. Returned len = %d.", len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Wrote %d of %d bytes.", len, strlen(claim_data->payload));
    len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if ((len > 0) && (status == 200)) {
        len = esp_http_client_read_response(client, claim_data->payload, sizeof(claim_data->payload));
        claim_data->payload[len] = '\0';
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_OK;
    } else {
        len = esp_http_client_read_response(client, claim_data->payload, sizeof(claim_data->payload));
        if (len >= 0) {
            claim_data->payload[len] = 0;
        }
        ESP_LOGE(TAG, "Invalid response for %s", url);
        ESP_LOGE(TAG, "Status = %d, Data = %s", status, len > 0 ? claim_data->payload : "None");
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
}
static esp_err_t esp_rmaker_claim_perform_init(esp_rmaker_claim_data_t *claim_data)
{
    esp_err_t err = generate_claim_init_request(claim_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate Claim init request");
        return err;
    }

    err = esp_rmaker_claim_perform_common(claim_data, CLAIM_INIT_PATH);
    if (err != OK) {
        ESP_LOGE(TAG, "Claim Init Request Failed.");
        return err;
    }
    claim_data->state = RMAKER_CLAIM_STATE_INIT;
    err = handle_self_claim_init_response(claim_data);
    if (err == ESP_OK) {
        claim_data->state = RMAKER_CLAIM_STATE_INIT_DONE;
    }
    return err;
}

static esp_err_t esp_rmaker_claim_perform_verify(esp_rmaker_claim_data_t *claim_data)
{
    esp_err_t err = esp_rmaker_claim_perform_common(claim_data, CLAIM_VERIFY_PATH);
    if (err != OK) {
        ESP_LOGE(TAG, "Claim Verify Failed.");
        return err;
    }
    claim_data->state = RMAKER_CLAIM_STATE_VERIFY;
    err = handle_claim_verify_response(claim_data);
    if (err == ESP_OK) {
        claim_data->state = RMAKER_CLAIM_STATE_VERIFY_DONE;
    }
    return err;
}

esp_err_t esp_rmaker_self_claim_perform(esp_rmaker_claim_data_t *claim_data)
{
    ESP_LOGI(TAG, "Starting the Self Claim Process. This may take time.");
    if (claim_data == NULL) {
        ESP_LOGE(TAG, "Self claiming not initialised.");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_rmaker_claim_perform_init(claim_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Claim Init Sequence Failed.");
        return err;
    }
    err = esp_rmaker_claim_perform_verify(claim_data);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Self Claiming was successful. Certificate received.");
    }
    esp_rmaker_claim_data_free(claim_data);
    return err;
}

esp_err_t __esp_rmaker_claim_init(esp_rmaker_claim_data_t *claim_data)
{
    esp_err_t err = ESP_OK;
    
    /* Generate the Private Key */
    err = esp_rmaker_claim_generate_key(claim_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate private key.");
        return err;
    }

    self_claim_private_key = calloc(1, strlen((char *)claim_data->payload) + 1);
    memcpy(self_claim_private_key, claim_data->payload, strlen((char *)claim_data->payload));
    err = esp_rmaker_claim_generate_csr(claim_data, self_claim_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate CSR.");
        return err;
    }
     /* New line characters from the CSR need to be removed and replaced with explicit \n for the claiming
     * service to parse properly. Make that change here and store the CSR in storage.
     */
    escape_new_line(claim_data);
    return err;
}

void esp_rmaker_claim_task(void *args)
{
    if (!args) {
        ESP_LOGE(TAG, "Arguments for claiming task cannot be NULL");
        return;
    }
    esp_rmaker_claim_data_t *claim_data = calloc(1, sizeof(esp_rmaker_claim_data_t));
    if (!claim_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for claim data.");
        return;
    }
    if (__esp_rmaker_claim_init(claim_data) != ESP_OK) {
        esp_rmaker_claim_data_free(claim_data);
    } else {
        *((esp_rmaker_claim_data_t **)args) = claim_data;
    }
    xEventGroupSetBits(claim_event_group, CLAIM_TASK_BIT);
    vTaskDelete(NULL);
}

static esp_rmaker_claim_data_t *esp_rmaker_claim_init(void)
{
    static bool claim_init_done;
    if (claim_init_done) {
        ESP_LOGE(TAG, "Claim already initialised");
        return NULL;
    }
    claim_event_group = xEventGroupCreate();
    if (!claim_event_group) {
        ESP_LOGE(TAG, "Couldn't create event group");
        return NULL;
    }
    esp_rmaker_claim_data_t *claim_data = NULL;

#define ESP_RMAKER_CLAIM_TASK_STACK_SIZE (10 * 1024)
    /* Using tskIDLE_PRIORITY so that the time consuming tasks, especially
     * PK generation does not trigger task WatchDog timer.
     */
    if (xTaskCreate(&esp_rmaker_claim_task, "claim_task", ESP_RMAKER_CLAIM_TASK_STACK_SIZE,
                &claim_data, tskIDLE_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Couldn't create Claim task");
        vEventGroupDelete(claim_event_group);
        return NULL;
    }

    /* Wait for claim init to complete */
    xEventGroupWaitBits(claim_event_group, CLAIM_TASK_BIT, false, true, portMAX_DELAY);
    vEventGroupDelete(claim_event_group);
    claim_event_group = NULL;
    return claim_data;
}
esp_rmaker_claim_data_t *esp_rmaker_self_claim_init(const char *name)
{
    self_claim_name = name;
    ESP_LOGI(TAG, "Initialising Self Claiming. This may take time.");
    return esp_rmaker_claim_init();
}

char *get_self_claim_certificate(void)
{
    return self_claim_certificate;
}

char *get_self_claim_private_key(void)
{
    return self_claim_private_key;
}