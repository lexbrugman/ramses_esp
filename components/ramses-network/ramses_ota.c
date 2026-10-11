/********************************************************************
 * ramses_esp
 * ramses_ota.c
 *
 * (C) 2023 Peter Price
 *
 * Over The Air upgrade
 *
 * Pull-based OTA from a GitHub release. CI publishes every branch to a
 * rolling "<branch>-latest" release (the OTA channel) holding the app image
 * and a version.txt with the commit SHA it was built from (see firmware.yml).
 *
 * The device checks version.txt shortly after boot, every few hours and on
 * request, and reports it as the latest version - it never installs on its
 * own. An install only downloads and flashes the image when version.txt
 * differs from the running version, unless forced from the console.
 */

#include <ctype.h>
#include <string.h>

static const char *TAG = "OTA";
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include "ota_cmd.h"
#include "ramses_ota.h"
#include "ramses_wifi.h"

// CI sets FW_OTA_REPO to the repo it runs in and FW_OTA_CHANNEL to the
// release it publishes the build to, so a binary keeps following the branch
// it was built from (see CMakeLists.txt).
#define OTA_RELEASES_URL   "https://github.com/" FW_OTA_REPO "/releases"
#define OTA_CHANNEL_URL    OTA_RELEASES_URL "/download/" FW_OTA_CHANNEL
#define OTA_RELEASE_URL    OTA_RELEASES_URL "/tag/" FW_OTA_CHANNEL

// The image built for this chip; never another target's, that would not boot
#define OTA_FIRMWARE_ASSET "ramses_esp_" CONFIG_IDF_TARGET ".bin"
#define OTA_VERSION_ASSET  "version.txt"

// Let WiFi and MQTT settle before the first check
#define OTA_STARTUP_DELAY  pdMS_TO_TICKS( 15*1000 )
#define OTA_RETRY_DELAY    pdMS_TO_TICKS( 60*1000 )
#define OTA_CHECK_INTERVAL pdMS_TO_TICKS( 6*60*60*1000 )

// Task notification bits
#define OTA_REQ_CHECK   0x01
#define OTA_REQ_INSTALL 0x02
#define OTA_REQ_FORCE   0x04

struct ota_data {
  BaseType_t coreID;
  TaskHandle_t task;

  char url[176];
  uint8_t force;

  esp_https_ota_handle_t ota_handle;

  portMUX_TYPE lock;
  struct ota_status status;
};

struct ota_data *ota_ctxt(void) {
  static struct ota_data Ctxt = {
    .url = OTA_CHANNEL_URL,

	.ota_handle = NULL,

	.lock = portMUX_INITIALIZER_UNLOCKED,
	.status = {
	  .seq = 1,
	  .percent = -1,
	},
  };
  static struct ota_data *ctxt = NULL;

  if( !ctxt ) {
	ctxt = &Ctxt;
	// Runtime initialisation?
  }

  return ctxt;
}

/********************************************************************
 * Status reporting
 */
static void ota_set_latest( struct ota_data *ctxt, char const *version ) {
  taskENTER_CRITICAL( &ctxt->lock );
  if( strcmp( ctxt->status.latest, version ) ) {
    strlcpy( ctxt->status.latest, version, sizeof(ctxt->status.latest) );
    ctxt->status.seq++;
  }
  taskEXIT_CRITICAL( &ctxt->lock );
}

static void ota_set_progress( struct ota_data *ctxt, bool in_progress, int8_t percent ) {
  taskENTER_CRITICAL( &ctxt->lock );
  if( ctxt->status.in_progress!=in_progress || ctxt->status.percent!=percent ) {
    ctxt->status.in_progress = in_progress;
    ctxt->status.percent = percent;
    ctxt->status.seq++;
  }
  taskEXIT_CRITICAL( &ctxt->lock );
}

/********************************************************************
 * Version check
 */
struct version_buf {
  char *data;
  size_t size;
  size_t len;
};

static esp_err_t _version_event_handler( esp_http_client_event_t *evt ) {
  struct version_buf *buf = evt->user_data;

  // Redirect responses carry a body too; only the final one is the version
  if( evt->event_id==HTTP_EVENT_ON_DATA && esp_http_client_get_status_code( evt->client )==200 ) {
    size_t n = evt->data_len;
    if( n > buf->size-1-buf->len ) n = buf->size-1-buf->len;
    memcpy( buf->data+buf->len, evt->data, n );
    buf->len += n;
    buf->data[buf->len] = '\0';
  }

  return ESP_OK;
}

static esp_err_t ota_fetch_version( struct ota_data *ctxt, char *version, size_t size ) {
  char url[sizeof(ctxt->url)+sizeof(OTA_VERSION_ASSET)+1];
  struct version_buf buf = { .data=version, .size=size, .len=0 };
  esp_http_client_config_t config = {
    .url = url,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .event_handler = _version_event_handler,
    .user_data = &buf,
    .timeout_ms = 8000,
	.buffer_size_tx = 4096,
  };

  snprintf( url, sizeof(url), "%s/%s", ctxt->url, OTA_VERSION_ASSET );
  version[0] = '\0';

  esp_http_client_handle_t client = esp_http_client_init( &config );
  if( !client ) {
    printf("# OTA: version check failed: no HTTP client\n");
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = esp_http_client_perform( client );
  int status = esp_http_client_get_status_code( client );
  esp_http_client_cleanup( client );

  if( err==ESP_OK && status!=200 ) {
    printf("# OTA: version check HTTP %d\n", status );
    err = ESP_FAIL;
  } else if( err!=ESP_OK ) {
    printf("# OTA: version check failed: %s\n", esp_err_to_name(err) );
  }

  if( err==ESP_OK ) {
    // Trim surrounding whitespace
    char *start = version;
    while( isspace( (unsigned char)*start ) ) start++;
    size_t len = strlen( start );
    while( len>0 && isspace( (unsigned char)start[len-1] ) ) len--;
    memmove( version, start, len );
    version[len] = '\0';

    if( len==0 ) {
      printf("# OTA: version check returned no version\n");
      err = ESP_FAIL;
    }
  }

  return err;
}

// Fetch the channel's published version and report it, whether or not it
// differs from the running one.
static esp_err_t ota_poll_version( struct ota_data *ctxt, char *version, size_t size ) {
  esp_err_t err = ota_fetch_version( ctxt, version, size );
  if( err==ESP_OK )
    ota_set_latest( ctxt, version );
  return err;
}

static esp_err_t ota_check_version( struct ota_data *ctxt ) {
  const esp_app_desc_t *app = esp_app_get_description();
  char remote[sizeof(ctxt->status.latest)];

  esp_err_t err = ota_poll_version( ctxt, remote, sizeof(remote) );
  if( err==ESP_OK ) {
    if( !strcmp( remote, app->version ) )
      printf("# OTA: up to date (%s)\n", app->version );
    else
      printf("# OTA: update available: %s -> %s\n", app->version, remote );
  }

  return err;
}

/********************************************************************
 * Install
 */
esp_err_t _http_event_handler( esp_http_client_event_t *evt ) {
  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
    break;
  case HTTP_EVENT_REDIRECT:
    ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
    break;
  default:
    break;
  }

  return ESP_OK;
}

static esp_err_t ota_begin( esp_https_ota_config_t  *config, struct ota_data *ctxt ) {
  esp_err_t err = esp_https_ota_begin( config, &ctxt->ota_handle );
  if( err!=ESP_OK ) {
    ESP_LOGE(TAG, "OTA Begin failed");
  }

  return err;
}

static esp_err_t ota_check_file_hdr( struct ota_data *ctxt ) {
  esp_app_desc_t app_info;
  esp_err_t err = esp_https_ota_get_img_desc( ctxt->ota_handle, &app_info );
  if( err != ESP_OK ) {
    ESP_LOGE(TAG, "esp_https_ota_read_img_desc failed");
  } else {
    esp_app_desc_t running_app_info;
    const esp_partition_t *running = esp_ota_get_running_partition();
    err = esp_ota_get_partition_description( running, &running_app_info );
	if( err==ESP_OK ) {
	  ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
	  printf("# OTA: Running firmware version: %s\n", running_app_info.version);
	}

	if( err==ESP_OK ) {
	  if( memcmp( app_info.version, running_app_info.version, sizeof(app_info.version) ) == 0 ) {
		if( ctxt->force ) {
          ESP_LOGW(TAG, "Current running version is the same as new. Update being forced.");
          printf("# OTA: Force update to %s\n",app_info.version);
		} else {
          ESP_LOGW(TAG, "Current running version is the same as new. We will not continue the update.");
          printf("# OTA: Target version is same as current version - no update\n");
	      err=ESP_FAIL;
		}
	  } else {
        ESP_LOGI(TAG, "Downloading firmware version %s",app_info.version);
        printf("# OTA: downloading firmware version %s\n",app_info.version);
	  }
	}
  }

  return err;
}

static esp_err_t ota_download_file( struct ota_data *ctxt ) {
  esp_err_t err;
  int size = esp_https_ota_get_image_size( ctxt->ota_handle );

  while (1) {
    err = esp_https_ota_perform( ctxt->ota_handle );
    if( err!=ESP_ERR_HTTPS_OTA_IN_PROGRESS ) {
      break;
    }

    if( size>0 ) {
      int read = esp_https_ota_get_image_len_read( ctxt->ota_handle );
      ota_set_progress( ctxt, true, (int8_t)( (int64_t)read*100/size ) );
    }
  }

  return err;
}

static esp_err_t ota_finish( struct ota_data *ctxt ) {
  esp_err_t err = ESP_FAIL;

  if( esp_https_ota_is_complete_data_received(ctxt->ota_handle) != true ) {
    // the OTA image was not completely received and user can customise the response to this situation.
    ESP_LOGE(TAG, "Complete data was not received.");
    printf("# OTA: complete data was not received\n");
  } else {
    err = esp_https_ota_finish( ctxt->ota_handle );
    if( err!=ESP_OK ) {
      if( err == ESP_ERR_OTA_VALIDATE_FAILED ) {
        ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        printf("# OTA: Image corrupted\n");
      } else {
        printf("# OTA: upgrde failed\n");
        ESP_LOGE(TAG, "upgrade failed 0x%x", err);
      }
    }
  }
  return err;
}

static void ota_install( struct ota_data *ctxt ) {
  const esp_app_desc_t *app = esp_app_get_description();
  esp_err_t err;

  // Downloading the image always reflashes whatever the channel holds, so
  // gate it on the published version to avoid a pointless reflash and reboot
  if( !ctxt->force ) {
    char remote[sizeof(ctxt->status.latest)];
    if( ota_poll_version( ctxt, remote, sizeof(remote) )!=ESP_OK ) {
      printf("# OTA: version unknown - not installing\n");
      return;
    }
    if( !strcmp( remote, app->version ) ) {
      printf("# OTA: already up to date (%s)\n", app->version );
      return;
    }
    printf("# OTA: installing %s -> %s\n", app->version, remote );
  }

  char url[256];
  esp_http_client_config_t config = {
    .url = url,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .event_handler = _http_event_handler,
    .keep_alive_enable = true,
	.buffer_size_tx = 4096,
  };
  esp_https_ota_config_t ota_config = {
    .http_config = &config,
  };

  snprintf( url, sizeof(url), "%s/%s", ctxt->url, OTA_FIRMWARE_ASSET );
  ESP_LOGI(TAG, "URL <%s>",url );

  ota_set_progress( ctxt, true, 0 );

  err = ota_begin( &ota_config, ctxt );
  if( err==ESP_OK ) {
                      err = ota_check_file_hdr(ctxt);
    if( err==ESP_OK ) err = ota_download_file(ctxt);
    if( err==ESP_OK ) err = ota_finish( ctxt );
    if( err!=ESP_OK ) esp_https_ota_abort( ctxt->ota_handle );
  }

  if( err==ESP_OK ) {
    printf("# OTA: Upgrade complete. Rebooting ...\n");
    ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful. Rebooting ...");
    vTaskDelay( 1000 / portTICK_PERIOD_MS );
    esp_restart();
  }

  ota_set_progress( ctxt, false, -1 );
}

/********************************************************************
 * OTA task
 */
static void Ota( void *param ) {
  struct ota_data *ctxt = param;
  // Requests are handled in between scheduled checks and don't move them
  TickType_t next_check = xTaskGetTickCount() + OTA_STARTUP_DELAY;

  ESP_LOGI( TAG, "Task Started");

  while( 1 ) {
    uint32_t req;
    int32_t remaining = (int32_t)( next_check - xTaskGetTickCount() );
    TickType_t wait = remaining>0 ? (TickType_t)remaining : 0;
    bool periodic = xTaskNotifyWait( 0, UINT32_MAX, &req, wait )==pdFALSE;
    if( periodic ) {
      req = OTA_REQ_CHECK;
      next_check = xTaskGetTickCount() + OTA_CHECK_INTERVAL;
    }

    // A scheduled check waiting for WiFi retries soon rather than a full
    // interval later; a failed fetch waits for the next interval
    if( !wifi_is_connected() ) {
      if( periodic ) next_check = xTaskGetTickCount() + OTA_RETRY_DELAY;
      else printf("# OTA: skipped, no WiFi\n");
      continue;
    }

    if( req & ( OTA_REQ_INSTALL|OTA_REQ_FORCE ) ) {
      ctxt->force = ( req & OTA_REQ_FORCE ) ? 1 : 0;
      ota_install( ctxt );
    } else {
      ota_check_version( ctxt );
    }
  }
}

/********************************************************************
 * External commands
 */
void ota_get_status( struct ota_status *status ) {
  struct ota_data *ctxt = ota_ctxt();
  taskENTER_CRITICAL( &ctxt->lock );
  *status = ctxt->status;
  taskEXIT_CRITICAL( &ctxt->lock );
}

char const *ota_channel( void ) {
  return FW_OTA_CHANNEL;
}

char const *ota_release_url( void ) {
  return OTA_RELEASE_URL;
}

void ota_set_url( const char * url ){
  struct ota_data *ctxt = ota_ctxt();
  strlcpy( ctxt->url, url, sizeof(ctxt->url) );
}

void ota_check( void ) {
  struct ota_data *ctxt = ota_ctxt();
  if( ctxt->task ) xTaskNotify( ctxt->task, OTA_REQ_CHECK, eSetBits );
}

void ota_start( uint8_t force ) {
  struct ota_data *ctxt = ota_ctxt();
  if( ctxt->task ) xTaskNotify( ctxt->task, force ? OTA_REQ_FORCE : OTA_REQ_INSTALL, eSetBits );
}

/********************************************************************/

void ramses_ota_init( BaseType_t coreID ) {
  struct ota_data *ctxt = ota_ctxt();
  ctxt->coreID = coreID;

  esp_log_level_set(TAG, CONFIG_OTA_LOG_LEVEL );

  ota_register();

  xTaskCreatePinnedToCore( Ota, "OTA", 8*1024, ctxt, 5, &ctxt->task, ctxt->coreID );
}
