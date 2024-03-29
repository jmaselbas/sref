/* SPDX-License-Identifier: BSD-2-Clause */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <locale.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>
#include <GL/glx.h>
#undef GL_TIMEOUT_IGNORED
#undef GL_INVALID_INDEX
#include "glad.h"

#include "stb_image.h"
#include "qoi.h"

#include "arg.h"

struct color {
	float r, g, b;
};

struct shortcut {
	unsigned int mod;
	KeySym keysym;
	void (*func)(void);
};

/* X modifiers */
#define XK_ANY_MOD	UINT_MAX
#define XK_NO_MOD	0

/* function definitions that can be used for shortcuts in config.h */
static void zoomreset(void);
static void saveboard(void);
static void toggleshape(void);

#include "config.h"

static int mod_match(unsigned int mask, unsigned int state)
{
	return mask == XK_ANY_MOD || mask == (state & ~ignoremod);
}

#define LEN(a) (sizeof(a)/sizeof(*a))
struct image {
	GLuint id;
	GLenum type;
	size_t width, height;
	int posx;
	int posy;
	float scale;
	const char *path;
};
static size_t image_count;
static struct image images[MAX_IMAGE_COUNT];

static struct image *hover_img;
static struct image *focus_img;
char *argv0;
static char *session_file;

static int orgx;
static int orgy;
static float zoom = 1;
static int mousex;
static int mousey;
static int xrel;
static int yrel;
static int lclick;
static int mclick;
static int rclick;

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
static Colormap map;
static Atom wmprotocols, wmdeletewin;

static XRectangle rect[LEN(images)];

static unsigned char dndversion = 3;
static Atom xdndaware, xdndenter, xdndposition, xdndstatus, xdndleave, xdnddrop, xdndfini;
static Atom xdndacopy, xdndselection, xdnddata, xdndtypelist;
static char *dndtargetnames[] = {
	"text/plain",
	"text/uri-list",
	"UTF8_STRING",
	"STRING",
	"TEXT",
};
static Atom dndtargetatoms[LEN(dndtargetnames)];
static Atom dndtarget;

static Cursor movecursor, grabcursor, scalecursor, defaultcursor;
static GLXContext ctx;

static GLuint quad_vao;
static GLuint quad_vbo;
static GLuint sprg;
static GLint loc_res;
static GLint loc_off;
static GLint loc_ext;
static GLint loc_img;

static char logbuf[4096];
static GLsizei logsize;

static void write_session(const char *name);
static void read_session(const char *name);

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(1);
}

static void
err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

char *
strdup(const char *s)
{
	size_t l = strlen(s);
	char *d = malloc(l + 1);
	if (d) memcpy(d, s, l + 1);
	return d;
}

static unsigned int
xtoi(char hex)
{
	return (isdigit(hex) ? hex - '0' : toupper(hex) - 'A' + 10);
}

static int
urldecode(char *str, char *url, size_t len)
{
	while (len > 0 && *url != '\0') {
		if (url[0] != '%') {
			*str++ = *url++;
		} else if (len > 2 && isxdigit(url[1]) && isxdigit(url[2])) {
			*str++ = 16 * xtoi(url[1]) + xtoi(url[2]);
			url += 3;
			len -= 3;
		} else {
			return 1; /* malformed url */
		}
	}
	if (len > 0)
		*str++ = '\0';

	return 0;
}

static void load_at(const char *name, int x, int y, float scale);
static void
load(const char *name)
{
	load_at(name, 0, 0, 1.0);
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
		"#version 300 es\n"
		"precision mediump float;\n"
		"layout(location = 0) in vec2 in_pos;\n"
		"out vec2 tex;\n"
		"uniform vec2 res;\n"
		"uniform vec2 off;\n"
		"uniform vec2 ext;\n"
		"uniform float scale;\n"
		"void main() {\n"
		"	vec2 pos = -1.0 + (in_pos * ext + off) * 2.0 / res;\n"
		"	gl_Position = vec4(pos.x, pos.y, 0.0, 1.0);\n"
		"	tex = vec2(in_pos.x, 1.0 - in_pos.y);\n"
		"}\n";
	const char *frag =
		"#version 300 es\n"
		"precision mediump float;\n"
		"in vec2 tex;\n"
		"out vec3 color;\n"
		"uniform sampler2D img;\n"
		"void main() {\n"
		"	color = texture(img, tex).rgb;\n"
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

static XRectangle
win_rect(void)
{
	XRectangle r = {
		.x = 0,
		.y = 0,
		.width = width,
		.height = height
	};

	return r;
}

static XRectangle
img_to_rect(struct image *i, int px)
{
	float z = zoom;
	int x = z * (i->posx + orgx) + width / 2;
	int y = z * (i->posy + orgy) + height / 2;
	int w = z * (i->width * i->scale);
	int h = z * (i->height * i->scale);
	XRectangle r = {
		.x = x - px,
		.y = y - px,
		.width = w + 2 * px,
		.height = h + 2 * px,
	};
	return r;
}

static int
mouse_in(int x, int y, int w, int h)
{
	int mx = mousex;
	int my = mousey;
	return (x <= mx && (x + w) >= mx) && (y <= my && (y + h) >= my);
}

static int
mouse_in_rect(XRectangle r)
{
	return mouse_in(r.x, r.y, r.width, r.height);
}

static int
mouse_in_img(struct image *i)
{
	return mouse_in_rect(img_to_rect(i, 0));
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
	XRectangle r = img_to_rect(i, 0);
	int x = r.x;
	int y = height - r.y - r.height;
	int w = r.width;
	int h = r.height;

	if (i == focus_img)
		glClearColor(focus.r, focus.g, focus.b, 1.0);
	else if (i == hover_img)
		glClearColor(hover.r, hover.g, hover.b, 1.0);
	else
		glClearColor(normal.r, normal.g, normal.b, 1.0);

	scissor(x, y, w, h, borderpx);
	glClear(GL_COLOR_BUFFER_BIT);

	glUniform2f(loc_off, x, y);
	glUniform2f(loc_ext, w, h);
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
	glClearColor(bg.r, bg.g, bg.b, bg_alpha);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(sprg);
	glBindVertexArray(quad_vao);
	glActiveTexture(GL_TEXTURE0 + 0);
	glUniform1i(loc_img, 0);
	glUniform2f(loc_res, width, height);

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

	if (!customshape || focus_img || image_count == 0) {
		XRectangle r = win_rect();
		XShapeCombineRectangles(dpy, win, ShapeBounding,
				0, 0, &r, 1, ShapeSet, 0);
	} else {
		for (i = 0; i < image_count; i++)
			rect[i] = img_to_rect(&images[i], borderpx);
		XShapeCombineRectangles(dpy, win, ShapeBounding,
				0, 0, rect, image_count, ShapeSet, 0);
	}

	glXSwapBuffers(dpy, win);
}

static int
select_visual(XVisualInfo *v)
{
	XRenderPictFormat *fmt;
	fmt = (void *)XRenderFindVisualFormat(dpy, v->visual);
	if (bg_alpha != 1.0 && fmt->direct.alphaMask == 0)
		return 0;

	return 1;
}

static int
glx_has_ext(const char *name)
{
	const char *exts = glXQueryExtensionsString(dpy, scr);
	return strstr(exts, name) != NULL;
}

static void
glx_init(void)
{
	typedef GLXContext (*glXCreateContextAttribsARB_f)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
	typedef void (*glXSwapIntervalEXT_f)(Display*, GLXDrawable, int);
	typedef void (*glXSwapIntervalSGI_f)(int);
	glXCreateContextAttribsARB_f glXCreateContextAttribsARB;
	glXSwapIntervalEXT_f glXSwapIntervalEXT;
	glXSwapIntervalSGI_f glXSwapIntervalSGI;
	XSetWindowAttributes wa = { 0 };
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
		GLX_CONTEXT_MINOR_VERSION_ARB, 0,
		None
	};
	GLXFBConfig *fbc;
	XVisualInfo *vis;
	int i, count;

	dpy = XOpenDisplay(NULL);
	if (!dpy)
		die("cannot open display\n");
	scr = DefaultScreen(dpy);

	glXQueryVersion(dpy, &maj, &min);
	if (maj <= 1 && min < 3)
		die("GLX 1.3 or greater is required\n");

	fbc = glXChooseFBConfig(dpy, scr, glx_attribs, &count);
	if (fbc == NULL || count <= 0)
		die("No framebuffer\n");

	for (i = 0; i < count; i++) {
		vis = glXGetVisualFromFBConfig(dpy, fbc[i]);
		if (!vis)
			continue;
		if (select_visual(vis))
			break;
		vis = NULL;
	}
	if (!vis)
		die("Could not create correct visual window\n");
	root = RootWindow(dpy, vis->screen);

	wa.background_pixel = 0;
	wa.border_pixel = 0;
	wa.colormap = map = XCreateColormap(dpy, root, vis->visual, AllocNone);
	wa.event_mask = ExposureMask | VisibilityChangeMask
		| FocusChangeMask | KeyPressMask | StructureNotifyMask
		| PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
	win = XCreateWindow(dpy, root, 0, 0, width, height, 0,
			    vis->depth, InputOutput, vis->visual,
			    CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &wa);
	if (!win)
		die("fail to create window\n");

	glXCreateContextAttribsARB = (glXCreateContextAttribsARB_f) glXGetProcAddress((const GLubyte *) "glXCreateContextAttribsARB");
	if (!glXCreateContextAttribsARB)
		die("Failed to load glXCreateContextAttribsARB\n");

	ctx = glXCreateContextAttribsARB(dpy, fbc[0], NULL, True, ctx_attribs);
	if (!ctx)
		die("Failed to create an openGL context\n");
	glXMakeCurrent(dpy, win, ctx);

	if (!gladLoadGLES2((GLADloadfunc) glXGetProcAddress))
		die("GL init failed\n");

	XFree(fbc);
	XFree(vis);

	if (glx_has_ext("GLX_EXT_swap_control")) {
		glXSwapIntervalEXT = (glXSwapIntervalEXT_f) glXGetProcAddress((const GLubyte *) "glXSwapIntervalEXT");
		if (glXSwapIntervalEXT)
			glXSwapIntervalEXT(dpy, glXGetCurrentDrawable(), 2);
	} else if (glx_has_ext("GLX_SGI_swap_control")) {
		glXSwapIntervalSGI = (glXSwapIntervalSGI_f) glXGetProcAddress((const GLubyte *) "glXSwapIntervalSGI");
		if (glXSwapIntervalSGI)
			glXSwapIntervalSGI(2);
	} else if (glx_has_ext("GLX_MESA_swap_control")) {
		err("FIXME: handle extension %s for vsync\n",
			"GLX_MESA_swap_control");
	}
}

static void
xdnd_init(void)
{
	xdndaware = XInternAtom(dpy, "XdndAware", False);
	xdndenter = XInternAtom(dpy, "XdndEnter", False);
	xdndacopy = XInternAtom(dpy, "XdndActionCopy", False);
	xdndposition = XInternAtom(dpy, "XdndPosition", False);
	xdndselection = XInternAtom(dpy, "XdndSelection", False);
	xdndtypelist = XInternAtom(dpy, "XdndTypeList", False);
	xdndstatus = XInternAtom(dpy, "XdndStatus", False);
	xdndleave = XInternAtom(dpy, "XdndLeave", False);
	xdnddrop = XInternAtom(dpy, "XdndDrop", False);
	xdndfini = XInternAtom(dpy, "XdndFinished", False);
	xdnddata = XInternAtom(dpy, "XDND_DATA", False);

	XInternAtoms(dpy, dndtargetnames, LEN(dndtargetnames), False, dndtargetatoms);

	XChangeProperty(dpy, win, xdndaware, XA_ATOM, 32, PropModeReplace, &dndversion, 1);
}

static void
x_init(void)
{
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!XSetLocaleModifiers(""))
		fputs("warning: no locale modifiers support\n", stderr);

	glx_init();

	wmprotocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmdeletewin = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wmdeletewin, 1);

	movecursor = XCreateFontCursor(dpy, XC_tcross);
	grabcursor = XCreateFontCursor(dpy, XC_hand1);
	scalecursor = XCreateFontCursor(dpy, XC_sizing);
	defaultcursor = XCreateFontCursor(dpy, XC_arrow);

	XStoreName(dpy, win, "sref");

	xdnd_init();

	XMapWindow(dpy, win);
	XSync(dpy, False);
}

static void
init(void)
{
	x_init();
	shader_init();
}

static void *
file_read(const char *name, size_t *s)
{
	char *data = NULL, *newp;
	size_t len, size;
	int n;
	int fd;

	fd = open(name, O_RDONLY);
	if (fd < 0)
		return NULL;

	len = size = 0;
	do {
		newp = realloc(data, size += 4096);
		if (!newp)
			goto free;
		data = newp;
		n = read(fd, &data[len], 4096);
		if (n < 0)
			goto free;
		len += n;
	} while (n == 4096);
	close(fd);

	*s = len;
	return data;
free:
	close(fd);
	free(data);
	return NULL;
}

static void
load_at(const char *name, int x, int y, float scale)
{
	GLenum format;
	int w, h, n;
	unsigned char *file;
	unsigned char *data;
	size_t len;
	int type_qoif = 0;

	if (name == NULL)
		return;

	if (image_count >= LEN(images)) {
		err("%s: Cannot open image, too many open\n", name);
		return;
	}

	file = file_read(name, &len);
	if (file == NULL || len == 0) {
		err("%s: Fail to load image\n", name);
		if (file)
			free(file);
		return;
	}

	if (len > 22 && strncmp((char *)file, "qoif", strlen("qoif")) == 0) {
		qoi_desc desc;
		type_qoif = 1;
		data = qoi_decode(file, len, &desc, 0);
		w = desc.width;
		h = desc.height;
		n = desc.channels;
	} else {
		data = stbi_load_from_memory(file, len, &w, &h, &n, 0);
	}
	free(file);
	if (data == NULL || n == 0) {
		err("%s: Fail to load image\n", name);
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
	images[image_count].path = strdup(name);
	images[image_count].scale = scale;
	images[image_count].posx = x - w / 2;
	images[image_count].posy = y - h / 2;
	image_count++;

	if (type_qoif) {
		free(data);
	} else {
		stbi_image_free(data);
	}
}

static void
resize(int w, int h)
{
	width  = (w < 0) ? 0 : w;
	height = (h < 0) ? 0 : h;
}

static void
zoomreset(void)
{
	zoom = 1;
}

static void
saveboard(void)
{
	write_session(session_file);
}

static void
toggleshape(void)
{
	customshape = !customshape;
}

static void *
xgetprop(Window w, Atom prop, Atom *type, int *fmt, size_t *cnt)
{
	unsigned long rem;
	void *ret = NULL;
	int r, size = 0;

	*type = None;
	*cnt = 0;
	do {
		if (ret != NULL)
			XFree(ret);
		r = XGetWindowProperty(dpy, w, prop, 0, size, False, AnyPropertyType,
				   type, fmt, cnt, &rem, (void *)&ret);
		if (r != Success)
			break;

		size += rem;
	} while (rem != 0);

	return ret;
}

static Atom
dndmatchtarget(size_t count, Atom *target)
{
	size_t t, i;

	for (t = 0; t < LEN(dndtargetatoms); t++)
		for (i = 0; i < count; i++)
			if (target[i] != None && target[i] == dndtargetatoms[t])
				return target[i];

	return None;
}

static void
xev_visnotify(XEvent *ev)
{
	if (ev->xvisibility.state != VisibilityUnobscured)
		XRaiseWindow(dpy, win);
}

static void
xev_selnotify(XEvent *e)
{
	unsigned long n;
	int fmt;
	char *uri, *data;
	Atom type, prop = None;

	if (e->type == SelectionNotify)
		prop = e->xselection.property;
	if (prop == None)
		return;
	data = xgetprop(win, prop, &type, &fmt, &n);
	if (!data)
		err("selection allocation failed\n");

	uri = strtok(data, "\r\n");
	while (uri != NULL) {
		if (strncmp(uri, "file://", strlen("file://")) == 0) {
			uri += strlen("file://");
			urldecode(uri, uri, strlen(uri) + 1);
			load(uri);
		}
		uri = strtok(NULL, "\r\n");
	}

	XFree(data);
	XDeleteProperty(dpy, win, prop);
}

static void
xev_keypress(XEvent *ev)
{
	XKeyEvent *e = &ev->xkey;
	unsigned int s = e->state;
	KeySym k = XLookupKeysym(e, 1);
	size_t i;

	for (i = 0; i < LEN(shortcuts); i++) {
		if (k == shortcuts[i].keysym && mod_match(shortcuts[i].mod, s))
			return shortcuts[i].func();
	}
}

static void
xev_cmessage(XEvent *ev)
{
	if (ev->xclient.message_type == xdndenter) {
		Window src = ev->xclient.data.l[0];
		int version = ev->xclient.data.l[1] >> 24;
		int typelist = ev->xclient.data.l[1] & 1;

		if (version < dndversion)
			err("unsupported dnd version %d\n", version);
		if (typelist) {
			Atom type = None;
			int fmt;
			Atom *data = NULL;
			unsigned long n;
			data = xgetprop(src, xdndtypelist, &type, &fmt, &n);
			dndtarget = dndmatchtarget(n, data);
			XFree(data);
		} else {
			dndtarget = dndmatchtarget(3, (Atom *) &ev->xclient.data.l[2]);
		}
	} else if (ev->xclient.message_type == xdndposition) {
		Window src = ev->xclient.data.l[0];
		Atom action = ev->xclient.data.l[4];
		/* accept the drag-n-drop if we matched a target,
		 * only xdndacopy action is supported */
		int accept = dndtarget != None && action == xdndacopy;
		XClientMessageEvent m = {
			.type = ClientMessage,
			.display = dpy,
			.window = src,
			.message_type = xdndstatus,
			.format = 32,
			.data.l = { win, accept, 0, 0, xdndacopy},
		};

		if (XSendEvent(dpy, src, False, NoEventMask, (XEvent *)&m) == 0)
			err("xsend error\n");
	} else if (ev->xclient.message_type == xdnddrop) {
		Time droptimestamp = ev->xclient.data.l[2];
		if (dndtarget != None)
			XConvertSelection(dpy, xdndselection, dndtarget, xdnddata, win, droptimestamp);
	} else if (ev->xclient.message_type == xdndleave) {
		dndtarget = None;
	}
}

static void
xev_resize(XEvent *ev)
{
	resize(ev->xconfigure.width, ev->xconfigure.height);
}

static void
xev_button(XEvent *ev)
{
	if (ev->xbutton.button == 4) /* mouse wheel up */
		zoom += zoom * 0.1 * (ev->type == ButtonPress);
	if (ev->xbutton.button == 5) /* mouse wheel down */
		zoom -= zoom * 0.1 * (ev->type == ButtonPress);
	if (ev->xbutton.button == 1)
		lclick = ev->type == ButtonPress;
	if (ev->xbutton.button == 2)
		mclick = ev->type == ButtonPress;
	if (ev->xbutton.button == 3)
		rclick = ev->type == ButtonPress;
}

static void
xev_motion(XEvent *ev)
{
	xrel -= mousex - ev->xmotion.x;
	yrel -= mousey - ev->xmotion.y;

	mousex = ev->xmotion.x;
	mousey = ev->xmotion.y;
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
			xev_keypress(&ev);
			break;
		case MotionNotify:
			xev_motion(&ev);
			break;
		case ButtonPress:
		case ButtonRelease:
			xev_button(&ev);
			break;
		case ConfigureNotify:
			xev_resize(&ev);
			break;
		case VisibilityNotify:
			xev_visnotify(&ev);
			break;
		case ClientMessage:
			if (ev.xclient.message_type == wmprotocols)
				return; /* assume wmdeletewin */
			xev_cmessage(&ev);
			break;
		case SelectionNotify:
			xev_selnotify(&ev);
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
			if (zoom > 100.0)
				zoom = 100.0;

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

char *
strtrim(char *s)
{
	while (*s != '\0' && isspace(*s)) s++;
	return s;
}

char *
argsplit(char *s)
{
	int esc = 0;
	char *p = s;

	while (*s != '\0' && (esc || !isspace(*s))) {
		char c = *s++;
		if (esc == '\\') { *p++ = c; esc = 0; continue; }
		if (c == esc)  { esc = 0; continue; }
		if (c == '\\') { esc = c; continue; }
		if (c == '\'') { esc = c; continue; }
		if (c == '"')  { esc = c; continue; }
		*p++ = c;
	}
	if (*s == '\0') {
		*p = '\0';
		return NULL; /* end of arg */
	}
	*p = '\0';
	return s + 1;
}

static void
open_file(size_t argc, const char **argv)
{
	const char *file;
	const char *a;
	float scale = 1.0;
	int x = 0, y = 0;
	size_t i;

	if (argc < 1)
		return;
	file = argv[0];
	for (i = 1; i < argc; i++) {
		a = argv[i];
		if (strncmp(a, "scale=", strlen("scale=")) == 0) {
			float f = strtof(a + strlen("scale="), NULL);
			if (f == HUGE_VALF || f == HUGE_VALL) {
				err("%s: %s\n", a, strerror(errno));
				return;
			}
			scale = f;
		}
		else if (strncmp(a, "x=", strlen("x=")) == 0) {
			long v = strtol(a + strlen("x="), NULL, 0);
			if (v == LONG_MIN || v == LONG_MAX) {
				err("%s: %s\n", a, strerror(errno));
				return;
			}
			x = v;
		}
		else if (strncmp(a, "y=", strlen("y=")) == 0) {
			long v = strtol(a + strlen("y="), NULL, 0);
			if (v == LONG_MIN || v == LONG_MAX) {
				err("%s: %s\n", a, strerror(errno));
				return;
			}
			y = v;
		}
	}
	load_at(file, x, y, scale);
}

static void
parse_line(char *l)
{
	const char *argv[16];
	size_t argc = 0;
	char *e;

	do {
		l = strtrim(l);
		if (*l == '\0') break;
		e = argsplit(l);
		argv[argc++] = l;
		l = e;
	} while (l != NULL && argc < LEN(argv));
	if (argc > 0)
		open_file(argc, argv);
}

static void
read_session(const char *name)
{
	char *line = NULL;
	size_t n = 0;
	ssize_t l;
	FILE *f;

	if (!name)
		return;
	f = fopen(name, "r");
	if (!f) {
		if (errno == ENOENT)
			err("%s: %s\n", name, strerror(errno));
		else
			die("%s: %s\n", name, strerror(errno));
		return;
	}

	while ((l = getline(&line, &n, f)) != -1) {
		char *p;
		p = strchr(line, '#');
		if (p) *p = '\0';
		p = strchr(line, '\n');
		if (p) *p = '\0';
		parse_line(line);
	}
	free(line);
	fclose(f);
}

static void
write_session(const char *name)
{
	size_t i;
	FILE *f;

	if (!name)
		return;
	f = fopen(name, "w");
	if (!f) {
		err("%s: %s\n", name, strerror(errno));
		return;
	}

	fprintf(f, "#!%s -f\n", argv0);
	for (i = 0; i < image_count; i++) {
		const char *p = images[i].path;
		int x = images[i].posx + images[i].width / 2;
		int y = images[i].posy + images[i].height / 2;
		float s = images[i].scale;
		fprintf(f, "'%s' x=%d y=%d scale=%f\n", p, x, y, s);
	}
	fclose(f);
}

static void
usage(void)
{
	printf("usage: %s [-hv] [--] [[+<X>x<Y>] files ...]\n", argv0);
	exit(1);
}

int
main(int argc, char **argv)
{
	int x, y;
	int i;

	ARGBEGIN {
	case 'v':
		err("%s %s\n", argv0, VERSION);
		exit(0);
	case 'f':
		session_file = EARGF(usage());
		break;
	case 'h':
	default:
		usage();
	} ARGEND;

	init();
	/* glX needs to be initialized */
	if (session_file)
		read_session(session_file);

	x = 0, y = 0;
	for (i = 0; i < argc; i++) {
		if (argv[i][0] == '+') {
			sscanf(argv[i], "+%dx%d", &x, &y);
			continue;
		}
		load_at(argv[i], x, y, 1.0);
		x = 0, y = 0;
	}

	run();

	glXMakeCurrent(dpy, 0, 0);
	glXDestroyContext(dpy, ctx);
	XDestroyWindow(dpy, win);
	XFreeColormap(dpy, map);
	XCloseDisplay(dpy);

	return 0;
}
