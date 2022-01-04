/* See LICENSE file for copyright and license details. */

static int borderpx = 1;
static struct color bg = {0.1, 0.1, 0.1};
static float bg_alpha = 1.0; /* set to != 1.0 to enable transparency */
static struct color normal = {0, 0, 0};
static struct color hover  = {0, 0x6b / 255.0, 0xcd / 255.0};
static struct color focus  = {0, 0x6b / 255.0, 0xcd / 255.0};

/*
 * Initial window size
 */
static unsigned int width = 1080;
static unsigned int height = 800;

/*
 * Enable custum window shape that only shows displayed images.
 * When enable, the background will not be visibe nor clickable
 * unless an image is clicked, disabled if there is no images.
 */
static int customshape = 1;

#define MAX_IMAGE_COUNT 1024
