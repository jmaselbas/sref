/* See LICENSE file for copyright and license details. */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>

#include "glad.h"
#include <SDL.h>
#include "stb_image.h"

#include "arg.h"

struct color {
	float r, g, b;
};

static int borderpx = 1;
static struct color bg = {0.1, 0.1, 0.1};
static struct color normal = {0, 0, 0};
static struct color hover  = {0, 0x6b / 255.0, 0xcd / 255.0}; /* #006bcd */
static struct color focus  = {0, 0x6b / 255.0, 0xcd / 255.0}; /* #006bcd */


#define LEN(a) (sizeof(a)/sizeof(*a))
struct image {
	GLuint id;
	GLenum type;
	size_t width, height;
	int posx;
	int posy;
	float scale;
};
size_t image_count;
struct image images[1024];

struct image *hover_img;
struct image *focus_img;
unsigned int width = 1080;
unsigned int height = 800;
char *argv0;

int orgx;
int orgy;
int zoom = 10;
int mousex;
int mousey;
int xrel;
int yrel;
int lclick;
int mclick;
int rclick;

enum action {
	NONE = 0,
	MOVE,
	SCALE,
	GRAB,
};
enum action act;

SDL_Window *window;
SDL_GLContext context;
SDL_Cursor *scale_cur;
SDL_Cursor *move_cur;
SDL_Cursor *grab_cur;

GLuint quad_vao;
GLuint quad_vbo;
GLuint sprg;
GLint loc_res;
GLint loc_off;
GLint loc_ext;
GLint loc_img;

char logbuf[4096];
GLsizei logsize;

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(1);
}

static struct image
create_image(size_t w, size_t h, GLenum format, GLenum type, void *data)
{
	struct image img = { 0 };

	img.type = GL_TEXTURE_2D;
	img.width = w;
	img.height = h;
	img.scale = 1;

	glGenTextures(1, &img.id);
	glBindTexture(img.type, img.id);

	glTexParameteri(img.type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(img.type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(img.type, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(img.type, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	/* for now input format is the same as the texture format */
	glTexImage2D(img.type, 0, format, w, h, 0, format, type, data);

	return img;
}

static void
shader_init(void)
{
	static float quad[] = {
		 0.0,  0.0,
		 0.0,  1.0,
		 1.0,  0.0,
		 1.0,  1.0,
	};
	const char *vert =
		"#version 330 core\n"
		"layout(location = 0) in vec2 in_pos;\n"
		"out vec2 tex;\n"
		"uniform vec2 res;\n"
		"uniform vec2 off;\n"
		"uniform vec2 ext;\n"
		"uniform float scale;\n"
		"void main()\n"
		"{\n"
		"	vec2 pos = -1 + (in_pos * ext + off) * 2 / res;\n"
		"	gl_Position = vec4(pos.x, pos.y, 0.0, 1.0);\n"
		"	tex = in_pos;\n"
		"}\n";
	const char *frag =
		"#version 330 core\n"
		"in vec2 tex;\n"
		"uniform sampler2D img;\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = texture(img, tex);\n"
		"}\n";
	GLint vert_size = strlen(vert);
	GLint frag_size = strlen(frag);
	GLuint vshd;
	GLuint fshd;
	GLint loc_in_pos = 0;
	int ret;

	vshd = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vshd, 1, &vert, &vert_size);
	glCompileShader(vshd);
	glGetShaderInfoLog(vshd, sizeof(logbuf), &logsize, logbuf);
	glGetShaderiv(vshd, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		glDeleteShader(vshd);
		printf("--- ERROR ---\n%s", logbuf);
		die("error in vertex shader\n");
	}

	fshd = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fshd, 1, (const GLchar * const*)&frag, &frag_size);
	glCompileShader(fshd);
	glGetShaderiv(fshd, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		glGetShaderInfoLog(fshd, sizeof(logbuf), &logsize, logbuf);
		glDeleteShader(vshd);
		glDeleteShader(fshd);
		printf("--- ERROR ---\n%s", logbuf);
		die("error in fragment shader\n");
	}

	sprg = glCreateProgram();
	glAttachShader(sprg, vshd);
	glAttachShader(sprg, fshd);
	glLinkProgram(sprg);
	glGetProgramiv(sprg, GL_LINK_STATUS, &ret);
	if (!ret) {
		glGetProgramInfoLog(sprg, sizeof(logbuf), &logsize, logbuf);
		glDeleteProgram(sprg);
		glDeleteShader(fshd);
		printf("--- ERROR ---\n%s", logbuf);
		die("error in shader link\n");
	}
	glUseProgram(sprg);

	loc_res = glGetUniformLocation(sprg, "res");
	loc_off = glGetUniformLocation(sprg, "off");
	loc_ext = glGetUniformLocation(sprg, "ext");
	loc_img = glGetUniformLocation(sprg, "img");

	glGenVertexArrays(1, &quad_vao);
	glBindVertexArray(quad_vao);

	glGenBuffers(1, &quad_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

	glVertexAttribPointer(loc_in_pos, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(loc_in_pos);
}

static void
input(void)
{
	SDL_Event e;
	int w, h;

	SDL_GL_GetDrawableSize(window, &w, &h);
	width  = (w < 0) ? 0 : w;
	height = (h < 0) ? 0 : h;

	xrel = yrel = 0;

	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			exit(0);
			break;
		case SDL_KEYDOWN:
			switch (e.key.keysym.sym) {
			case SDLK_0:
				zoom = 10;
				break;
			}
			break;
		case SDL_MOUSEMOTION:
			mousex = e.motion.x;
			mousey = height - e.motion.y;
			xrel += e.motion.xrel;
			yrel -= e.motion.yrel;
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			if (e.button.button == SDL_BUTTON_MIDDLE)
				mclick = e.button.state == SDL_PRESSED;
			if (e.button.button == SDL_BUTTON_LEFT)
				lclick = e.button.state == SDL_PRESSED;
			if (e.button.button == SDL_BUTTON_RIGHT)
				rclick = e.button.state == SDL_PRESSED;
			break;
		case SDL_MOUSEWHEEL:
			zoom += e.wheel.y;
			if (zoom < 0)
				zoom = 0;
			break;
		case SDL_KEYUP:
		case SDL_WINDOWEVENT:
			break;
		}
	}

	if (lclick) {
		if (act != MOVE)
			SDL_SetCursor(move_cur);
		act = MOVE;
	} else if (rclick) {
		if (act != MOVE)
			SDL_SetCursor(scale_cur);
		act = SCALE;
	} else if (mclick) {
		if (act != GRAB)
			SDL_SetCursor(grab_cur);
		act = GRAB;
	} else {
		if (act != NONE)
			SDL_SetCursor(SDL_GetDefaultCursor());
		act = NONE;
	}

	xrel /= 0.1 * zoom;
	yrel /= 0.1 * zoom;
	if (act == GRAB) {
		orgx += xrel;
		orgy += yrel;
	}
}

static int
mouse_in(int x, int y, int w, int h)
{
	int mx = mousex;
	int my = mousey;
	return (x <= mx && (x + w) >= mx) && (y <= my && (y + h) >= my);
}

static int
mouse_in_img(struct image *i)
{
	float z = 0.1 * zoom;
	int x = z * (i->posx + orgx) + width / 2;
	int y = z * (i->posy + orgy) + height / 2;
	int w = z * (i->width * i->scale);
	int h = z * (i->height * i->scale);

	return mouse_in(x, y, w, h);
}

static void
scissor(int x, int y, int w, int h, int px)
{
	if (x > 0) {
		x -= px;
		w += px;
	}
	if (y > 0) {
		y -= px;
		h += px;
	}
	glScissor(x, y, w + px, h + px);
}

static void
render_img(struct image *i)
{
	float z = 0.1 * zoom;
	int x = z * (i->posx + orgx) + width / 2;
	int y = z * (i->posy + orgy) + height / 2;
	int w = z * (i->width * i->scale);
	int h = z * (i->height * i->scale);

	if (i == focus_img)
		glClearColor(focus.r, focus.g, focus.b, 1.0);
	else if (i == hover_img)
		glClearColor(hover.r, hover.g, hover.b, 1.0);
	else
		glClearColor(normal.r, normal.g, normal.b, 1.0);

	scissor(x, y, w, h, borderpx);
	glClear(GL_COLOR_BUFFER_BIT);

	glProgramUniform2f(sprg, loc_off, x, y);
	glProgramUniform2f(sprg, loc_ext, w, h);
	glBindTexture(i->type, i->id);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void
update(void)
{
	size_t i;

	glEnable(GL_SCISSOR_TEST);
	glViewport(0, 0, width, height);
	glScissor(0, 0, width, height);
	glClearColor(bg.r, bg.g, bg.b, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(sprg);
	glBindVertexArray(quad_vao);
	glActiveTexture(GL_TEXTURE0 + 0);
	glUniform1i(loc_img, 0);
	glProgramUniform2f(sprg, loc_res, width, height);

	hover_img = NULL;
	if (act == NONE)
		focus_img = NULL;
	for (i = 0; i < image_count; i++) {
		if (mouse_in_img(&images[i])) {
			hover_img = &images[i];
		}
	}
	if (act != NONE && focus_img == NULL)
		focus_img = hover_img;
	if (focus_img) {
		switch (act) {
		case MOVE:
			focus_img->posx += xrel;
			focus_img->posy += yrel;
			break;
		case SCALE:
			focus_img->scale += 0.01 * xrel;
			if (focus_img->scale < 0.01)
				focus_img->scale = 0.01;
			break;
		default:
			break;
		}
	}

	for (i = 0; i < image_count; i++)
		render_img(&images[i]);

	SDL_GL_SwapWindow(window);
}

static void
init(void)
{
	if (SDL_InitSubSystem(SDL_INIT_VIDEO))
		die("SDL init failed: %s\n", SDL_GetError());

	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, SDL_TRUE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

	window = SDL_CreateWindow(argv0, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
				  width, height, SDL_WINDOW_OPENGL
				  | SDL_WINDOW_RESIZABLE
				  );

	if (!window)
		die("Failed to create window: %s\n", SDL_GetError());

	context = SDL_GL_CreateContext(window);
	if (!context)
		die("Failed to create openGL context: %s\n", SDL_GetError());

	SDL_GL_SetSwapInterval(1);

	if (!gladLoadGLLoader((GLADloadproc) SDL_GL_GetProcAddress))
		die("GL init failed\n");

	scale_cur = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
	move_cur = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);
	grab_cur = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);

	shader_init();
}

static void
load(const char *name)
{
	GLenum format;
	int w, h, n;
	unsigned char *data;

	if (name == NULL)
		return;

	if (image_count >= LEN(images)) {
		fprintf(stderr, "%s: Cannot open image, too many open\n", name);
		return;
	}

	stbi_set_flip_vertically_on_load(1);
	data = stbi_load(name, &w, &h, &n, 0);

	if (data == NULL || n == 0) {
		fprintf(stderr, "%s: Fail to load image\n", name);
		return;
	}

	if (n == 1)
		format = GL_RED;
	else if (n == 2)
		format = GL_RG;
	else if (n == 3)
		format = GL_RGB;
	else if (n == 4)
		format = GL_RGBA;
	else
		format = GL_RED;

	images[image_count] = create_image(w, h, format, GL_UNSIGNED_BYTE, data);
	image_count++;

	stbi_image_free(data);
}

static void
usage(void)
{
	printf("usage: %s [image_files ...]\n", argv0);
	exit(1);
}

int
main(int argc, char **argv)
{
	int i;

	ARGBEGIN {
	default:
		usage();
	} ARGEND;

	init();

	for (i = 0; i < argc; i++)
		load(argv[i]);

	while (1) {
		input();
		update();
	}

	return 0;
}
