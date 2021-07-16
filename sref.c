/* See LICENSE file for copyright and license details. */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <locale.h>

#include "glad.h"
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <GL/glx.h>
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
float zoom = 1;
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

static int scr;
static Display *dpy;
static Window root, win;
Atom wmprotocols, wmdeletewin;
Cursor movecursor, grabcursor, scalecursor, defaultcursor;
GLXContext ctx;

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

static void load_at(const char *name, int x, int y);
static void
load(const char *name)
{
	load_at(name, 0, 0);
}

static struct image
create_image(size_t w, size_t h, GLenum format, GLenum type, void *data)
{
	struct image img = { 0 };
	GLint rrr1[] = {GL_RED, GL_RED, GL_RED, GL_ONE};
	GLint rrra[] = {GL_RED, GL_RED, GL_RED, GL_ALPHA};
	GLint rgb1[] = {GL_RED, GL_GREEN, GL_BLUE, GL_ONE};
	GLint rgba[] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
	GLint *swiz = rrr1;

	img.type = GL_TEXTURE_2D;
	img.width = w;
	img.height = h;
	img.scale = 1;

	glGenTextures(1, &img.id);
	glBindTexture(img.type, img.id);

	if (format == GL_RED)
		swiz = rrr1;
	else if (format == GL_RG)
		swiz = rrra;
	else if (format == GL_RGB)
		swiz = rgb1;
	else if (format == GL_RGBA)
		swiz = rgba;

	glTexParameteriv(img.type, GL_TEXTURE_SWIZZLE_RGBA, swiz);
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
		"	vec2 pos = -1.0 + (in_pos * ext + off) * 2.0 / res;\n"
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
	float z = zoom;
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
	float z = zoom;
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

	glXSwapBuffers(dpy, win);
}

static void
glx_init(void)
{
	GLXContext (*glXCreateContextAttribsARB)(Display*, GLXFBConfig, GLXContext, Bool, const int*) = NULL;
	GLint maj, min;
	int glx_attribs[] = {
		GLX_X_RENDERABLE,   True,
		GLX_X_VISUAL_TYPE,  GLX_TRUE_COLOR,
		GLX_DRAWABLE_TYPE,  GLX_WINDOW_BIT,
		GLX_RENDER_TYPE,    GLX_RGBA_BIT,
		GLX_RED_SIZE,       8,
		GLX_GREEN_SIZE,     8,
		GLX_BLUE_SIZE,      8,
		GLX_ALPHA_SIZE,     8,
		GLX_DOUBLEBUFFER,   True,
		None
	};
	int ctx_attribs[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
		GLX_CONTEXT_MINOR_VERSION_ARB, 3,
		None
	};
	GLXFBConfig *fbc;
	XVisualInfo *vis;
	int count;

	glXQueryVersion(dpy, &maj, &min);
	if (maj <= 1 && min < 3)
		die("GLX 1.3 or greater is required.\n");

	fbc = glXChooseFBConfig(dpy, scr, glx_attribs, &count);
	if (fbc == NULL || count <= 0)
		die("No framebuffer\n");

	vis = glXGetVisualFromFBConfig(dpy, fbc[0]);
	if (!vis)
		die("Could not create correct visual window.\n");

	glXCreateContextAttribsARB = (void *) glXGetProcAddressARB((const GLubyte *) "glXCreateContextAttribsARB");
	if (!glXCreateContextAttribsARB)
		die("Failed to load glXCreateContextAttribsARB\n");

	ctx = glXCreateContextAttribsARB(dpy, fbc[0], 0, True, ctx_attribs);
	glXMakeCurrent(dpy, win, ctx);

	if (!gladLoadGLLoader((GLADloadproc) glXGetProcAddress))
		die("GL init failed\n");

	XFree(fbc);
	XFree(vis);
}

static void
x_init(void)
{
	XSetWindowAttributes wa = { 0 };
//	wa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
	wa.background_pixel = 0x191919;
	wa.event_mask = ExposureMask | VisibilityChangeMask
		| FocusChangeMask | KeyPressMask | StructureNotifyMask
		| PointerMotionMask | ButtonPressMask | ButtonReleaseMask;

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!XSetLocaleModifiers(""))
		fputs("warning: no locale modifiers support\n", stderr);

	dpy = XOpenDisplay(NULL);
	if (!dpy)
		die("cannot open display");

	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	win = XCreateWindow(dpy, root, 0, 0, width, height, 0,
	                    CopyFromParent, CopyFromParent, CopyFromParent,
	                    CWBackPixel | CWEventMask, &wa);
	if (!win)
		die("fail to create window");

	wmprotocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmdeletewin = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wmdeletewin, 1);

	movecursor = XCreateFontCursor(dpy, XC_tcross);
	grabcursor = XCreateFontCursor(dpy, XC_hand1);
	scalecursor = XCreateFontCursor(dpy, XC_sizing);
	defaultcursor = XCreateFontCursor(dpy, XC_arrow);

	XStoreName(dpy, win, "sref");

	/* do opengl init before XMapWindow */
	glx_init();

	XMapWindow(dpy, win);
	XSync(dpy, False);
}

static void
init(void)
{
	x_init();
	shader_init();
}

static void
load_at(const char *name, int x, int y)
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
	images[image_count].posx = x - w / 2;
	images[image_count].posy = y - h / 2;
	image_count++;

	stbi_image_free(data);
}

static void
resize(int w, int h)
{
	width  = (w < 0) ? 0 : w;
	height = (h < 0) ? 0 : h;
}

static void
run(void)
{
	XEvent ev;

	update();
	xrel = yrel = 0;

	while (!XNextEvent(dpy, &ev)) {

		if (!XFilterEvent(&ev, None))
		switch (ev.type) {
		case KeyPress:
			switch (XLookupKeysym(&ev.xkey, 1)) {
			case XK_0:
			case XK_KP_0:
			case XK_Home:
				zoom = 1;
				break;
			}
			break;
		case MotionNotify:
			xrel -= mousex - ev.xmotion.x;
			yrel -= mousey - (height - ev.xmotion.y);

			mousex = ev.xmotion.x;
			mousey = height - ev.xmotion.y;
			break;
		case ButtonPress:
		case ButtonRelease:
			if (ev.xbutton.button == 4) /* mouse wheel up */
				zoom += zoom * 0.1 * (ev.type == ButtonPress);
			if (ev.xbutton.button == 5) /* mouse wheel down */
				zoom -= zoom * 0.1 * (ev.type == ButtonPress);
			if (ev.xbutton.button == 1)
				lclick = ev.type == ButtonPress;
			if (ev.xbutton.button == 2)
				mclick = ev.type == ButtonPress;
			if (ev.xbutton.button == 3)
				rclick = ev.type == ButtonPress;
			break;
		case ConfigureNotify:
			resize(ev.xconfigure.width, ev.xconfigure.height);
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		case ClientMessage:
			if (ev.xclient.message_type == wmprotocols) {
				/* assume wmdeletewin */
				return;
			}
			break;
		default:
			break;
		}

		if (XPending(dpy) == 0) {
			if (lclick) {
				if (act != MOVE)
					XDefineCursor(dpy, win, movecursor);
				act = MOVE;
			} else if (rclick) {
				if (act != SCALE)
					XDefineCursor(dpy, win, scalecursor);
				act = SCALE;
			} else if (mclick) {
				if (act != GRAB)
					XDefineCursor(dpy, win, grabcursor);
				act = GRAB;
			} else {
				if (act != NONE)
					XDefineCursor(dpy, win, defaultcursor);
				act = NONE;
			}

			if (zoom < 0.01)
				zoom = 0.01;

			xrel /= zoom;
			yrel /= zoom;
			if (act == GRAB) {
				orgx += xrel;
				orgy += yrel;
			}

			update();
			xrel = yrel = 0;
		}
	}
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

	run();

	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);

	return 0;
}
