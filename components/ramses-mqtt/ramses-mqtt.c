/********************************************************************
 * ramses_esp
 * ramses-mqtt.c
 *
 * (C) 2023 Peter Price
 *
 * MQTT Client
 *
 */
#include <string.h>

static const char *TAG = "MQTT";
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

#include "mqtt_client.h"
#include "cJSON.h"

#include "cmd.h"
#include "device.h"
#include "gateway.h"
#include "ramses_ota.h"
#include "ramses_wifi.h"
#include "ramses-mqtt.h"

/****************************************************
 * States
 */
#define MQTT_STATE_LIST \
  MQTT_STATE( MQTT_IDLE,	      "Idle" ) \
  MQTT_STATE( MQTT_WAIT,          "Wait" ) \
  MQTT_STATE( MQTT_START,         "Start" ) \
  MQTT_STATE( MQTT_STARTING,      "Starting" ) \
  MQTT_STATE( MQTT_CONNECTED,     "Connected" ) \
  MQTT_STATE( MQTT_ACTIVE,        "Active" ) \

#define MQTT_STATE( _e, _t ) _e,
enum mqtt_state {
  MQTT_STATE_LIST
  MQTT_STATE_MAX
};
#undef MQTT_STATE

static char const *mqtt_state_text( enum mqtt_state state ) {
  static char const * const state_text[MQTT_STATE_MAX] = {
    #define MQTT_STATE( _e,_t ) _t,
    MQTT_STATE_LIST
    #undef MQTT_STATE
  };

  char const *text = "Unknown";
  if( state<MQTT_STATE_MAX )
    text = state_text[state];

  return text;
}

struct mqtt_data {
  BaseType_t coreID;
  TaskHandle_t task;

  enum mqtt_state state;
  char root[20];
  char topic[32];

  esp_mqtt_client_config_t cfg;
  esp_mqtt_client_handle_t client;

  uint8_t info;
  uint32_t ota_seq;	// Last OTA status published, 0 forces a publish

  volatile bool ha_online;	// Home Assistant (re)started, set from the event handler
  int64_t uptime_due;	// esp_timer time to publish the uptime, 0 when none due
};

static struct mqtt_data *mqtt_ctxt( void ) {
  static struct mqtt_data mqtt = {
    .state = MQTT_IDLE,
	.root = CONFIG_MQTT_ROOT,
    .cfg = {
      .session = {
    	.last_will = {
          .msg = "offline",
          .topic = mqtt.topic,
          .qos = 1,
          .retain = 1,
    	},
		.keepalive=20,
      },
    },
	.info = 0,
  };

  static struct mqtt_data *ctxt = NULL;
  if( !ctxt ){
    ctxt = &mqtt;
    // Runtime Initialisation?
  }

  return ctxt;
}

static void log_error_if_nonzero( const char *message, int error_code ) {
  if (error_code != 0) {
    ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code );
  }
}

static bool mqtt_broker_configured( struct mqtt_data *ctxt ) {
  bool res;

  // Most recent config
  char const * uri = NET_get_mqtt_broker();
  char const * user = NET_get_mqtt_user();
  char const * password = NET_get_mqtt_password();

  if( uri ) ESP_LOGI( TAG, "broker=<%s>",uri);
  if( user ) ESP_LOGI( TAG, "user=<%s>",user);
  if( password ) ESP_LOGI( TAG, "password=<%s>",password);

  res = uri  && uri[0] != '\0';
  if( res ) {  // Have URI, are we trying to authenticate
	if( user || password ) {
	  res = user && password; // Need both
	  if( res ) res = user[0] != '\0';
	  if( res ) res = password[0] != '\0';
	}
  }

  if( res ) {
    ctxt->cfg.broker.address.uri = uri;
    ctxt->cfg.credentials.username = user;
    ctxt->cfg.credentials.authentication.password = password;
  }

  return res;
}

static void mqtt_set_state( struct mqtt_data *ctxt, enum mqtt_state newState ) {
  if( ctxt ) {
    if( ctxt->state != newState ) {
      ESP_LOGI( TAG, "state %s->%s ", mqtt_state_text(ctxt->state), mqtt_state_text(newState) );
      ctxt->state = newState;
    }
  }
}

/****************************************************
 * Information topics
 */

typedef void (*info_field)( struct mqtt_data *ctxt, char *topic );

enum info_topic {
  INFO_FW,
  INFO_VER,
  // last
  INFO_MAX
};

static void publish_firmware( struct mqtt_data *ctxt, char *topic ) {
  const esp_app_desc_t *app = esp_app_get_description();
  strcat( topic, "/firmware" );
  esp_mqtt_client_publish( ctxt->client,topic, app->project_name, 0, 1, 1);
}

static void publish_version( struct mqtt_data *ctxt, char *topic ) {
  const esp_app_desc_t *app = esp_app_get_description();
  strcat( topic, "/version" );
  esp_mqtt_client_publish( ctxt->client,topic, app->version, 0, 1, 1);
}

static void mqtt_publish_info( struct mqtt_data *ctxt ) {
  static info_field const publish[INFO_MAX] = {
    [INFO_FW]  = publish_firmware,
	[INFO_VER] = publish_version,
  };
  if( ctxt->info < INFO_MAX ) {
    char topic[64];
    sprintf( topic, "%s/info", ctxt->topic );
    publish[ctxt->info]( ctxt, topic );
    ctxt->info++;
  }
}

// Give Home Assistant time to set up the entities after discovery, or after
// it (re)starts, before publishing state it has to receive
#define HA_SETTLE_US ( 5*1000000LL )

/*******************************************************************************
 * Home Assistant discovery
 *
 * Registers the gateway as a Home Assistant device with a firmware update
 * entity, a last restart sensor, plus buttons to check for updates and
 * restart. The buttons' commands are plain console commands sent to the cmd
 * topic.
 *
 * Retained discovery configs under this device's node ID for entities not in
 * HA_ENTITY_LIST are deleted so Home Assistant removes those entities.
 */

// Every entity mqtt_publish_discovery() publishes: id, component, object
#define HA_ENTITY_LIST \
  HA_ENTITY( HA_FIRMWARE,     "update", "firmware" ) \
  HA_ENTITY( HA_LAST_RESTART, "sensor", "last_restart" ) \
  HA_ENTITY( HA_CHECK_UPDATE, "button", "check_update" ) \
  HA_ENTITY( HA_RESTART,      "button", "restart" ) \

#define HA_ENTITY( _e, _c, _o ) _e,
enum ha_entity_id {
  HA_ENTITY_LIST
  HA_ENTITY_MAX
};
#undef HA_ENTITY

#define HA_ENTITY( _e, _c, _o ) [_e] = { _c, _o },
static struct ha_entity {
  char const *component;
  char const *object;
} const ha_entities[HA_ENTITY_MAX] = {
  HA_ENTITY_LIST
};
#undef HA_ENTITY

static void mqtt_ha_id( char *id, size_t size ) {
  // Discovery IDs only allow [a-zA-Z0-9_-]; the device ID looks like 18:123456
  snprintf( id, size, "ramses_esp_%s", device() );
  for( char *c=id; *c; c++ )
    if( *c==':' ) *c = '_';
}

static void mqtt_ha_publish( struct mqtt_data *ctxt, enum ha_entity_id entity, cJSON *json ) {
  char const *component = ha_entities[entity].component;
  char const *object = ha_entities[entity].object;
  const esp_app_desc_t *app = esp_app_get_description();
  char id[32], unique_id[64], name[48], model[48], topic[128];

  mqtt_ha_id( id, sizeof(id) );
  snprintf( unique_id, sizeof(unique_id), "%s_%s", id, object );
  snprintf( name, sizeof(name), "RAMSES ESP %s", device() );
  snprintf( model, sizeof(model), "%s (%s)", app->project_name, CONFIG_IDF_TARGET );
  snprintf( topic, sizeof(topic), "%s/%s/%s/%s/config", CONFIG_MQTT_HA_DISCOVERY_PREFIX, component, id, object );

  cJSON_AddStringToObject( json, "unique_id", unique_id );
  cJSON_AddStringToObject( json, "availability_topic", ctxt->topic );

  cJSON *dev = cJSON_AddObjectToObject( json, "device" );
  cJSON *ids = cJSON_AddArrayToObject( dev, "identifiers" );
  cJSON_AddItemToArray( ids, cJSON_CreateString( id ) );
  cJSON_AddStringToObject( dev, "name", name );
  cJSON_AddStringToObject( dev, "model", model );
  cJSON_AddStringToObject( dev, "sw_version", app->version );

  char *config = cJSON_PrintUnformatted( json );
  if( config ) {
    esp_mqtt_client_publish( ctxt->client, topic, config, 0, 1, 1 );
    cJSON_free( config );
  }
  cJSON_Delete( json );
}

static void mqtt_ha_button( struct mqtt_data *ctxt, enum ha_entity_id entity, char const *name,
                            char const *device_class, char const *icon, char const *press ) {
  char cmd_topic[64];
  snprintf( cmd_topic, sizeof(cmd_topic), "%s/cmd/cmd", ctxt->topic );

  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject( json, "name", name );
  if( device_class ) cJSON_AddStringToObject( json, "device_class", device_class );
  if( icon ) cJSON_AddStringToObject( json, "icon", icon );
  cJSON_AddStringToObject( json, "entity_category", "config" );
  cJSON_AddStringToObject( json, "command_topic", cmd_topic );
  cJSON_AddStringToObject( json, "payload_press", press );
  mqtt_ha_publish( ctxt, entity, json );
}

static void mqtt_publish_discovery( struct mqtt_data *ctxt ) {
  char cmd_topic[64], state_topic[64], latest_topic[64];
  snprintf( cmd_topic, sizeof(cmd_topic), "%s/cmd/cmd", ctxt->topic );
  snprintf( state_topic, sizeof(state_topic), "%s/ota/state", ctxt->topic );
  snprintf( latest_topic, sizeof(latest_topic), "%s/ota/latest", ctxt->topic );

  // installed_version is not a discovery field for this platform (HA drops
  // unknown keys); it comes from the state topic's JSON, see mqtt_publish_ota
  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject( json, "name", "Firmware" );
  cJSON_AddStringToObject( json, "device_class", "firmware" );
  cJSON_AddStringToObject( json, "entity_category", "config" );
  cJSON_AddStringToObject( json, "state_topic", state_topic );
  cJSON_AddStringToObject( json, "latest_version_topic", latest_topic );
  cJSON_AddStringToObject( json, "command_topic", cmd_topic );
  cJSON_AddStringToObject( json, "payload_install", "ota start" );
  cJSON_AddStringToObject( json, "title", ota_channel() );
  cJSON_AddStringToObject( json, "release_url", ota_release_url() );
  mqtt_ha_publish( ctxt, HA_FIRMWARE, json );

  // The gateway has no reliable clock, so it publishes its uptime and Home
  // Assistant turns that into the moment it restarted. Only sent on connect
  // and when Home Assistant restarts, see mqtt_publish_uptime
  char uptime_topic[64];
  snprintf( uptime_topic, sizeof(uptime_topic), "%s/uptime", ctxt->topic );
  json = cJSON_CreateObject();
  cJSON_AddStringToObject( json, "name", "Last restart" );
  cJSON_AddStringToObject( json, "device_class", "timestamp" );
  cJSON_AddStringToObject( json, "entity_category", "diagnostic" );
  cJSON_AddStringToObject( json, "state_topic", uptime_topic );
  cJSON_AddStringToObject( json, "value_template",
    "{{ (now() - timedelta(seconds=value|int)).replace(microsecond=0).isoformat() }}" );
  mqtt_ha_publish( ctxt, HA_LAST_RESTART, json );

  // Re-reads the OTA channel's version now instead of waiting for the next
  // periodic check; never installs
  mqtt_ha_button( ctxt, HA_CHECK_UPDATE, "Check for updates", NULL, "mdi:cloud-search", "ota check" );
  mqtt_ha_button( ctxt, HA_RESTART, "Restart", "restart", NULL, "reset" );
}

static void mqtt_subscribe_ha( struct mqtt_data *ctxt ) {
  char id[32], topic[128];
  mqtt_ha_id( id, sizeof(id) );

  // Home Assistant's birth message
  snprintf( topic, sizeof(topic), "%s/status", CONFIG_MQTT_HA_DISCOVERY_PREFIX );
  esp_mqtt_client_subscribe( ctxt->client, topic, 0 );

  // Replays every retained discovery config under this device's node ID
  snprintf( topic, sizeof(topic), "%s/+/%s/+/config", CONFIG_MQTT_HA_DISCOVERY_PREFIX, id );
  esp_mqtt_client_subscribe( ctxt->client, topic, 0 );
}

static bool mqtt_ha_known( char const *component, char const *object ) {
  for( int i=0; i<HA_ENTITY_MAX; i++ ) {
    if( !strcmp( ha_entities[i].component, component ) && !strcmp( ha_entities[i].object, object ) )
      return true;
  }
  return false;
}

// subtopic is the topic below the discovery prefix
static void mqtt_process_ha( struct mqtt_data *ctxt, char *subtopic, esp_mqtt_event_handle_t event ) {
  if( !strcmp( subtopic, "status" ) ) {
    if( event->data_len==6 && !strncmp( event->data, "online", 6 ) )
      ctxt->ha_online = true;
    return;
  }

  // An empty config is a deletion, possibly our own
  if( event->total_data_len==0 )
    return;

  // <component>/<node id>/<object>/config
  char *component = subtopic;
  char *node = strchr( component, '/' );
  char *object = node ? strchr( node+1, '/' ) : NULL;
  char *config = object ? strchr( object+1, '/' ) : NULL;
  if( !config || strcmp( config, "/config" ) )
    return;
  *node++ = '\0';
  *object++ = '\0';
  *config = '\0';

  char id[32];
  mqtt_ha_id( id, sizeof(id) );
  if( strcmp( node, id ) || mqtt_ha_known( component, object ) )
    return;

  char topic[128];
  snprintf( topic, sizeof(topic), "%s/%s/%s/%s/config", CONFIG_MQTT_HA_DISCOVERY_PREFIX, component, node, object );
  esp_mqtt_client_publish( ctxt->client, topic, NULL, 0, 1, 1 );
  printf("# MQTT: removed retired Home Assistant entity %s/%s\n", component, object );
}

static void mqtt_publish_ota( struct mqtt_data *ctxt ) {
  struct ota_status status;
  ota_get_status( &status );
  if( status.seq==ctxt->ota_seq )
    return;
  ctxt->ota_seq = status.seq;

  const esp_app_desc_t *app = esp_app_get_description();
  char topic[64];

  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject( json, "installed_version", app->version );
  cJSON_AddBoolToObject( json, "in_progress", status.in_progress );
  if( status.percent>=0 )
    cJSON_AddNumberToObject( json, "update_percentage", status.percent );
  else
    cJSON_AddNullToObject( json, "update_percentage" );

  char *state = cJSON_PrintUnformatted( json );
  if( state ) {
    sprintf( topic, "%s/ota/state", ctxt->topic );
    esp_mqtt_client_publish( ctxt->client, topic, state, 0, 1, 1 );
    cJSON_free( state );
  }
  cJSON_Delete( json );

  if( status.latest[0] ) {
    sprintf( topic, "%s/ota/latest", ctxt->topic );
    esp_mqtt_client_publish( ctxt->client, topic, status.latest, 0, 1, 1 );
  }
}

// Not retained: Home Assistant converts the uptime to a timestamp using the
// time it receives it, so a replayed value would give the wrong restart time.
static void mqtt_publish_uptime( struct mqtt_data *ctxt ) {
  int64_t now = esp_timer_get_time();

  if( ctxt->ha_online ) {
    ctxt->ha_online = false;
    ctxt->uptime_due = now + HA_SETTLE_US;
  }
  if( !ctxt->uptime_due || now < ctxt->uptime_due )
    return;
  ctxt->uptime_due = 0;

  char topic[64], uptime[24];
  sprintf( topic, "%s/uptime", ctxt->topic );
  sprintf( uptime, "%lld", now/1000000 );
  esp_mqtt_client_publish( ctxt->client, topic, uptime, 0, 1, 0 );
}

/*******************************************************************************
 * RX message
 */
static void mqtt_publish_rx( struct mqtt_data *ctxt, char const *ts, char const *msg ) {
  cJSON *json = cJSON_CreateObject();
  char *rx;
  char topic[64];

  char *end = strstr( msg,"\r\n");
  if( end ) end[0] = '\0';

  cJSON_AddStringToObject( json, "msg", msg );
  cJSON_AddStringToObject( json, "ts",  ts );

  rx = cJSON_Print( json );
  sprintf( topic, "%s/rx", ctxt->topic );
  esp_mqtt_client_publish( ctxt->client,topic, rx, 0, 1, 0 );

  cJSON_free( rx );
  cJSON_Delete( json );
}

void MQTT_publish_rx( char const *ts, char const *msg ) {
  struct mqtt_data *ctxt= mqtt_ctxt();
  if( ctxt->state == MQTT_ACTIVE )
    mqtt_publish_rx( ctxt, ts, msg );
}

/*******************************************************************************
 * TX message
 */
static void mqtt_subscribe_tx( struct mqtt_data *ctxt ) {
  char topic[64];
  sprintf( topic, "%s/tx", ctxt->topic );
  esp_mqtt_client_subscribe( ctxt->client, topic, 0);
}

static void mqtt_process_tx( struct mqtt_data *ctxt, char const *data, int dataLen ) {
  cJSON *json, *msg;

  ESP_LOGI( TAG, "TX:<%.*s> %s", dataLen,data,esp_log_system_timestamp() );

  json = cJSON_Parse( data );
  msg = cJSON_GetObjectItem( json, "msg" );

  ESP_LOGD( TAG, "<%s>", msg->valuestring );
  gateway_tx( msg->valuestring );

  cJSON_Delete( json );
}

/*******************************************************************************
 * General command
 */

static void mqtt_publish_cmd( struct mqtt_data *ctxt ) {
  char topic[64];
  sprintf( topic, "%s/cmd/cmd",ctxt->topic );
  esp_mqtt_client_publish( ctxt->client,topic, NULL, 0, 0, 0);
}


static void mqtt_subscribe_cmd( struct mqtt_data *ctxt ) {
  char topic[64];
  sprintf( topic, "%s/cmd/cmd", ctxt->topic );
  esp_mqtt_client_subscribe( ctxt->client, topic, 2 );
}

static void mqtt_publish_cmd_result( struct mqtt_data *ctxt, char const *cmd, esp_err_t err, int retVal ) {
  char topic[192],data[128];

  sprintf( topic, "%s/cmd/result",ctxt->topic );
  sprintf( data, "{ \"cmd\":\"%s\", \"err\":\"%s\", \"return\":%d} ",cmd, esp_err_to_name(err),retVal );
  esp_mqtt_client_publish( ctxt->client,topic, data, 0, 0, 0 );

//  mqtt_publish_cmd( ctxt ); // Clear CMD so we don't execute ita again
}

static void mqtt_process_cmd( struct mqtt_data *ctxt, char const *data, int dataLen ) {
  if( dataLen>0 ) {
    char cmdline[128];
    sprintf( cmdline,"%.*s", dataLen,data );	// Make sure cmdline contains trailing '\0'

    int retVal;
    esp_err_t err = cmd_run( cmdline, &retVal );

    mqtt_publish_cmd_result( ctxt, cmdline, err, retVal );
  }
}

/*******************************************************************************
 * Process subscribed topics
 */

enum topics {
  TOPIC_TX,
  TOPIC_CMD,
  TOPIC_MAX
};

static void mqtt_process_data( struct mqtt_data *ctxt, esp_mqtt_event_handle_t event ) {
  static struct topic {
	char const *topic;
	void (*func)( struct mqtt_data *ctxt, char const *data, int dataLen );
  } const topic_list[TOPIC_MAX+1] = {
    [TOPIC_TX]  = { "tx",      mqtt_process_tx },
    [TOPIC_CMD] = { "cmd",     mqtt_process_cmd },
    // List terminator
    [TOPIC_MAX] = { NULL,NULL },
  };
  struct topic const *topic = topic_list;

  // Only the first fragment of a large message carries the topic; none of
  // the handlers need more than that
  if( event->current_data_offset!=0 )
    return;

  size_t ha_len = strlen( CONFIG_MQTT_HA_DISCOVERY_PREFIX );
  if( event->topic_len>ha_len && !strncmp( event->topic, CONFIG_MQTT_HA_DISCOVERY_PREFIX "/", ha_len+1 ) ) {
    char ha_topic[128];
    if( event->topic_len < sizeof(ha_topic) ) {
      sprintf( ha_topic, "%.*s", event->topic_len-ha_len-1, event->topic+ha_len+1 );
      mqtt_process_ha( ctxt, ha_topic, event );
    }
    return;
  }

  size_t offset = strlen( ctxt->topic );
  char const *subtopic = event->topic + offset + 1;

  while( topic->topic != NULL ) {
    if( !strncmp( subtopic, topic->topic, strlen(topic->topic) ) ) {
      if( topic->func )
    	( topic->func )( ctxt, event->data, event->data_len );
      break;
    }
    topic++;
  }

  if( topic->topic == NULL )
    ESP_LOGI(TAG, "Unexpected event %.*s",event->topic_len-offset-1, subtopic );

}

static void mqtt_event_handler( void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data )
{
  struct mqtt_data *ctxt = handler_args;

  ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
  esp_mqtt_event_handle_t event = event_data;
//  esp_mqtt_client_handle_t client = event->client;
  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    mqtt_set_state( ctxt, MQTT_CONNECTED );
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    printf("# MQTT: Disonnected\n");
    break;

  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    break;

  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
    break;

  case MQTT_EVENT_PUBLISHED:
    ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d %s", event->msg_id,esp_log_system_timestamp());
    break;

  case MQTT_EVENT_DATA:
    mqtt_process_data(ctxt, event);
	break;

  case MQTT_EVENT_ERROR:
    ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
    if( event->error_handle->error_type==MQTT_ERROR_TYPE_TCP_TRANSPORT ) {
      log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
      log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
      log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
      ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
    }
    break;

  default:
    ESP_LOGI(TAG, "Other event id:%d", event->event_id);
    break;
  }
}

static void mqtt_state_machine( struct mqtt_data *ctxt ) {

  switch( ctxt->state ){
  case MQTT_IDLE:
    if( mqtt_broker_configured( ctxt ) ) {
    	mqtt_set_state( ctxt,MQTT_WAIT );
    }
    break;

  case MQTT_WAIT:
	if( wifi_is_connected() )
      mqtt_set_state( ctxt,MQTT_START );
	break;

  case MQTT_START:
	char const *dev = device();
	if( dev[0] ) {
      sprintf( ctxt->topic,"%s/%s",ctxt->root,dev );
      ctxt->client = esp_mqtt_client_init( &ctxt->cfg );
      ESP_LOGI( TAG, "Connecting to %s",ctxt->cfg.broker.address.uri );
      printf("# MQTT: Connecting to %s\n",ctxt->cfg.broker.address.uri );
      esp_mqtt_client_register_event( ctxt->client, ESP_EVENT_ANY_ID, mqtt_event_handler, ctxt );
      esp_mqtt_client_start( ctxt->client );
      mqtt_set_state( ctxt,MQTT_STARTING );
	}
    break;

  case MQTT_STARTING:
	break;

  case MQTT_CONNECTED:
    printf("# MQTT: Connected\n");
    esp_mqtt_client_publish( ctxt->client, ctxt->topic, "online", 0, 1, 1);
    mqtt_subscribe_tx( ctxt );
    mqtt_publish_cmd( ctxt );  // Clear old CMD
    mqtt_subscribe_cmd( ctxt );
    mqtt_publish_discovery( ctxt );
    mqtt_subscribe_ha( ctxt );
    ctxt->ota_seq = 0;
    ctxt->uptime_due = esp_timer_get_time() + HA_SETTLE_US;
	mqtt_set_state( ctxt,MQTT_ACTIVE );
    break;

  case MQTT_ACTIVE:
    if( ctxt->info<INFO_MAX )
      mqtt_publish_info( ctxt );
    mqtt_publish_ota( ctxt );
    mqtt_publish_uptime( ctxt );
    break;

  default:
    break;
  };
}

static void Mqtt( void *param ) {
  struct mqtt_data *ctxt = param;

  ESP_LOGI( TAG, "Task Started");

  do {
	mqtt_state_machine( ctxt );
    vTaskDelay( 50/portTICK_PERIOD_MS );
  } while(1);
}

MQTT_HNDL ramses_mqtt_init( BaseType_t coreID ) {
  struct mqtt_data *ctxt= mqtt_ctxt();
  ctxt->coreID = coreID;

  esp_log_level_set(TAG, CONFIG_MQTT_LOG_LEVEL );

  xTaskCreatePinnedToCore( Mqtt, "MQTT", 4096, ctxt, 10, &ctxt->task, ctxt->coreID );

  return ctxt;
}
