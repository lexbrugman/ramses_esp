/********************************************************************
 * ramses_esp
 * ota_cmd.c
 *
 * (C) 2023 Peter Price
 *
 * OTA Commands
 *
 */
#include "cmd.h"
#include "ota_cmd.h"
#include "ramses_ota.h"

/*********************************************************
 * URL command
 */
static int ota_cmd_url( int argc, char **argv ) {

  if( argc>1 )
    ota_set_url( argv[1] );

  return 0;
}

/*********************************************************
 * Check command
 */
static int ota_cmd_check( int argc, char **argv ) {

  ota_check();

  return 0;
}

/*********************************************************
 * Start command
 */
static int ota_cmd_start( int argc, char **argv ) {

  ota_start(0);

  return 0;
}

static int ota_cmd_force( int argc, char **argv ) {

  ota_start(1);

  return 0;
}

/*********************************************************
 * Top Level command
 */
static esp_console_cmd_t const ota_cmds[] = {
  {
    .command = "url",
    .help = "Set OTA channel <url>",
    .hint = NULL,
    .func = ota_cmd_url,
  },
  {
    .command = "check",
    .help = "Check for a new version",
    .hint = NULL,
    .func = ota_cmd_check,
  },
  {
    .command = "start",
    .help = "Start OTA",
    .hint = NULL,
    .func = ota_cmd_start,
  },
  {
    .command = "force",
    .help = "Force OTA",
    .hint = NULL,
    .func = ota_cmd_force,
  },
  // List termination
  { NULL_COMMAND }
};

static int ota_cmd( int argc, char **argv ) {
  return cmd_menu( argc, argv, ota_cmds, argv[0] );
}

void ota_register(void) {
  const esp_console_cmd_t ota[] = {
    {
      .command = "ota",
      .help = "OTA commands, enter 'ota' for list",
	  .hint = NULL,
	  .func = &ota_cmd,
    },
	{ NULL_COMMAND }
  };

  cmd_menu_register( ota );
}
