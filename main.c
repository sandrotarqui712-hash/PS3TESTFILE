/*
 * Gestor de archivos para PS3 (CFW/HEN) - PSL1GHT
 * v1.0
 *
 * Controles (explorador):
 *   Arriba/Abajo ... mover        L1/R1 ... pagina arriba/abajo
 *   X .............. abrir carpeta
 *   O .............. subir un nivel
 *   Cuadrado ....... marcar/desmarcar
 *   Triangulo ...... menu (copiar, cortar, pegar, borrar, nueva carpeta)
 *   SELECT ......... marcar todo / desmarcar todo
 *   START .......... salir
 *
 * Seguridad: /dev_flash*, /dev_blind y /dev_bdvd son de SOLO LECTURA.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ppu-types.h>
#include <sys/process.h>
#include <io/pad.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>

#include "font8x16.h"

SYS_PROCESS_PARAM(1001, 0x100000)

/* ------------------------------------------------------------------ */
/* Constantes                                                          */
/* ------------------------------------------------------------------ */

#define MAXPATH      1024
#define CB_SIZE      0x100000
#define HOST_SIZE    (32 * 1024 * 1024)
#define COPY_BUF     (1024 * 1024)

#define COL_BG       0x00101820u
#define COL_BAR      0x00203040u
#define COL_TEXT     0x00E0E0E0u
#define COL_DIM      0x00889098u
#define COL_DIR      0x007FC8FFu
#define COL_SEL      0x002A5080u
#define COL_MARK     0x00FFD060u
#define COL_OK       0x0070E090u
#define COL_ERR      0x00FF7070u
#define COL_BOX      0x00182838u
#define COL_BORDER   0x007FC8FFu

enum {
    BTN_UP_M     = 1 << 0,
    BTN_DOWN_M   = 1 << 1,
    BTN_LEFT_M   = 1 << 2,
    BTN_RIGHT_M  = 1 << 3,
    BTN_CROSS_M  = 1 << 4,
    BTN_CIRCLE_M = 1 << 5,
    BTN_SQUARE_M = 1 << 6,
    BTN_TRI_M    = 1 << 7,
    BTN_L1_M     = 1 << 8,
    BTN_R1_M     = 1 << 9,
    BTN_START_M  = 1 << 10,
    BTN_SELECT_M = 1 << 11
};

enum { MODE_BROWSE, MODE_MENU, MODE_CONFIRM_DEL };

/* ------------------------------------------------------------------ */
/* Estado de video                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int       width, height, id;
    uint32_t *ptr;
    uint32_t  offset;
} Buffer;

static gcmContextData *context = NULL;
static Buffer          bufs[2];
static int             cur = 0;         /* buffer en el que se dibuja   */
static int             first_flip = 1;
static int             scale = 1;       /* 1 = 8x16, 2 = 16x32          */
static int             CW, CH;          /* tamano de celda en pixeles   */
static int             cols, rows;      /* celdas en pantalla           */

static volatile int running = 1;

static void sysutil_cb(u64 status, u64 param, void *userdata)
{
    (void)param; (void)userdata;
    if (status == SYSUTIL_EXIT_GAME)
        running = 0;
}

static void wait_flip(void)
{
    while (gcmGetFlipStatus() != 0)
        usleep(200);
    gcmResetFlipStatus();
}

static void flip_buffer(int id)
{
    if (!first_flip)
        wait_flip();
    else
        gcmResetFlipStatus();
    first_flip = 0;

    if (gcmSetFlip(context, id) == 0) {
        rsxFlushBuffer(context);
        gcmSetWaitFlip(context);
    }
}

static void make_buffer(Buffer *b, u16 w, u16 h, int id)
{
    int pitch = sizeof(u32) * w;
    b->ptr = (uint32_t *)rsxMemalign(64, (u32)h * pitch);
    if (!b->ptr)
        exit(1);
    rsxAddressToOffset(b->ptr, &b->offset);
    gcmSetDisplayBuffer(id, b->offset, pitch, w, h);
    b->width  = w;
    b->height = h;
    b->id     = id;
}

static void init_video(void)
{
    void *host = memalign(1024 * 1024, HOST_SIZE);
    videoState state;
    videoConfiguration vconfig;
    videoResolution res;

    rsxInit(&context, CB_SIZE, HOST_SIZE, host);

    videoGetState(0, 0, &state);
    videoGetResolution(state.displayMode.resolution, &res);

    memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution = state.displayMode.resolution;
    vconfig.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch      = res.width * sizeof(u32);
    vconfig.aspect     = state.displayMode.aspect;

    videoConfigure(0, &vconfig, NULL, 0);
    videoGetState(0, 0, &state);

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    make_buffer(&bufs[0], res.width, res.height, 0);
    make_buffer(&bufs[1], res.width, res.height, 1);

    gcmResetFlipStatus();

    scale = (res.height >= 720) ? 2 : 1;
    CW = 8 * scale;
    CH = 16 * scale;
    cols = res.width / CW;
    rows = res.height / CH;
}

/* ------------------------------------------------------------------ */
/* Dibujo                                                              */
/* ------------------------------------------------------------------ */

static void fill_rect(int x, int y, int w, int h, uint32_t color)
{
    Buffer *b = &bufs[cur];
    int x1 = x + w, y1 = y + h, i, j;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > b->width)  x1 = b->width;
    if (y1 > b->height) y1 = b->height;
    for (j = y; j < y1; j++) {
        uint32_t *row = b->ptr + (size_t)j * b->width;
        for (i = x; i < x1; i++)
            row[i] = color;
    }
}

static void draw_glyph(int px, int py, unsigned cp, uint32_t color)
{
    Buffer *b = &bufs[cur];
    const unsigned char *g;
    int gy, gx, sy, sx;

    if (cp < 32 || cp > 255)
        cp = '?';
    g = font8x16[cp - 32];

    for (gy = 0; gy < 16; gy++) {
        unsigned char bits = g[gy];
        if (!bits)
            continue;
        for (gx = 0; gx < 8; gx++) {
            if (!(bits & (0x80 >> gx)))
                continue;
            for (sy = 0; sy < scale; sy++) {
                int yy = py + gy * scale + sy;
                if (yy < 0 || yy >= b->height) continue;
                for (sx = 0; sx < scale; sx++) {
                    int xx = px + gx * scale + sx;
                    if (xx < 0 || xx >= b->width) continue;
                    b->ptr[(size_t)yy * b->width + xx] = color;
                }
            }
        }
    }
}

/* Dibuja texto UTF-8 (solo hasta Latin-1) en la celda (cx, cy).
 * Devuelve cuantas celdas ha ocupado. maxc limita la longitud. */
static int draw_text(int cx, int cy, uint32_t color, const char *s, int maxc)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;

    while (*p && n < maxc) {
        unsigned cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else {
            /* secuencia mas larga o invalida: saltar toda la secuencia */
            p++;
            while ((*p & 0xC0) == 0x80) p++;
            cp = '?';
        }
        draw_glyph((cx + n) * CW, cy * CH, cp, color);
        n++;
    }
    return n;
}

static void draw_text_right(int cy, uint32_t color, const char *s, int right_col)
{
    int len = (int)strlen(s);
    draw_text(right_col - len, cy, color, s, len);
}

static void fill_cells(int cx, int cy, int cw, int ch, uint32_t color)
{
    fill_rect(cx * CW, cy * CH, cw * CW, ch * CH, color);
}

static void draw_box(int cx, int cy, int cw, int ch)
{
    fill_cells(cx, cy, cw, ch, COL_BORDER);
    fill_rect(cx * CW + 3, cy * CH + 3, cw * CW - 6, ch * CH - 6, COL_BOX);
}

/* ------------------------------------------------------------------ */
/* Utilidades                                                          */
/* ------------------------------------------------------------------ */

typedef unsigned long long u64l;

static void fmt_size(u64l v, char *out, size_t n)
{
    if (v < 1024ULL)
        snprintf(out, n, "%llu B", v);
    else if (v < 1024ULL * 1024)
        snprintf(out, n, "%.1f KB", v / 1024.0);
    else if (v < 1024ULL * 1024 * 1024)
        snprintf(out, n, "%.1f MB", v / (1024.0 * 1024.0));
    else
        snprintf(out, n, "%.2f GB", v / (1024.0 * 1024.0 * 1024.0));
}

static void path_join(char *out, size_t n, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, n, "/%s", name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int is_directory(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Rutas del sistema o de solo lectura: no se permite escribir. */
static int is_protected(const char *path)
{
    return strncmp(path, "/dev_flash", 10) == 0 ||
           strncmp(path, "/dev_blind", 10) == 0 ||
           strncmp(path, "/dev_bdvd", 9) == 0;
}

/* ------------------------------------------------------------------ */
/* Entrada (mando)                                                     */
/* ------------------------------------------------------------------ */

static unsigned read_pad(void)
{
    padInfo  info;
    padData  d;
    unsigned b = 0;
    int i;

    ioPadGetInfo(&info);
    for (i = 0; i < MAX_PADS; i++) {
        if (!info.status[i])
            continue;
        ioPadGetData(i, &d);
        if (d.BTN_UP)       b |= BTN_UP_M;
        if (d.BTN_DOWN)     b |= BTN_DOWN_M;
        if (d.BTN_LEFT)     b |= BTN_LEFT_M;
        if (d.BTN_RIGHT)    b |= BTN_RIGHT_M;
        if (d.BTN_CROSS)    b |= BTN_CROSS_M;
        if (d.BTN_CIRCLE)   b |= BTN_CIRCLE_M;
        if (d.BTN_SQUARE)   b |= BTN_SQUARE_M;
        if (d.BTN_TRIANGLE) b |= BTN_TRI_M;
        if (d.BTN_L1)       b |= BTN_L1_M;
        if (d.BTN_R1)       b |= BTN_R1_M;
        if (d.BTN_START)    b |= BTN_START_M;
        if (d.BTN_SELECT)   b |= BTN_SELECT_M;
    }
    return b;
}

/* ------------------------------------------------------------------ */
/* Lista de archivos                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char    name[256];
    int     is_dir;
    u64l    size;
    int     marked;
} Entry;

static Entry *ents = NULL;
static int    nents = 0, capents = 0;
static char   cwd[MAXPATH] = "/dev_hdd0";
static int    sel = 0, top = 0;

static char   status_msg[256] = "";
static uint32_t status_col = COL_DIM;

static void set_status(uint32_t col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(uint32_t col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_msg, sizeof(status_msg), fmt, ap);
    va_end(ap);
    status_col = col;
}

static void add_entry(const char *name, int is_dir, u64l size)
{
    if (nents == capents) {
        int ncap = capents ? capents * 2 : 256;
        Entry *n = (Entry *)realloc(ents, ncap * sizeof(Entry));
        if (!n)
            return;
        ents = n;
        capents = ncap;
    }
    memset(&ents[nents], 0, sizeof(Entry));
    snprintf(ents[nents].name, sizeof(ents[nents].name), "%s", name);
    ents[nents].is_dir = is_dir;
    ents[nents].size   = size;
    nents++;
}

static int cmp_entries(const void *a, const void *b)
{
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void load_dir(void)
{
    nents = 0;

    if (strcmp(cwd, "/") == 0) {
        static const char *roots[] = {
            "dev_hdd0", "dev_hdd1", "dev_usb000", "dev_usb001", "dev_usb002",
            "dev_usb003", "dev_bdvd", "dev_flash", "dev_flash2", "dev_flash3",
            "dev_blind", "host_root", NULL
        };
        int i;
        for (i = 0; roots[i]; i++) {
            char p[64];
            snprintf(p, sizeof(p), "/%s", roots[i]);
            if (is_directory(p))
                add_entry(roots[i], 1, 0);
        }
    } else {
        DIR *d = opendir(cwd);
        struct dirent *de;
        if (!d) {
            set_status(COL_ERR, "No se puede abrir %s (errno %d)", cwd, errno);
        } else {
            while ((de = readdir(d)) != NULL) {
                char p[MAXPATH];
                struct stat st;
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                path_join(p, sizeof(p), cwd, de->d_name);
                if (stat(p, &st) == 0)
                    add_entry(de->d_name, S_ISDIR(st.st_mode), (u64l)st.st_size);
                else
                    add_entry(de->d_name, 0, 0);
            }
            closedir(d);
            qsort(ents, nents, sizeof(Entry), cmp_entries);
        }
    }

    if (sel >= nents) sel = nents - 1;
    if (sel < 0) sel = 0;
    top = 0;
}

static void enter_dir(const char *newpath)
{
    snprintf(cwd, sizeof(cwd), "%s", newpath);
    sel = 0;
    top = 0;
    status_msg[0] = 0;
    load_dir();
}

static void go_up(void)
{
    char prev[256];
    char *slash;
    int i;

    if (strcmp(cwd, "/") == 0)
        return;

    snprintf(prev, sizeof(prev), "%s", base_name(cwd));
    slash = strrchr(cwd, '/');
    if (slash == cwd)
        cwd[1] = 0;
    else
        *slash = 0;

    status_msg[0] = 0;
    load_dir();
    for (i = 0; i < nents; i++) {
        if (!strcmp(ents[i].name, prev)) {
            sel = i;
            break;
        }
    }
}

static int count_marked(void)
{
    int i, n = 0;
    for (i = 0; i < nents; i++)
        n += ents[i].marked;
    return n;
}

/* ------------------------------------------------------------------ */
/* Portapapeles                                                        */
/* ------------------------------------------------------------------ */

static char **clip = NULL;
static int    nclip = 0;
static int    clip_move = 0;

static void clip_clear(void)
{
    int i;
    for (i = 0; i < nclip; i++)
        free(clip[i]);
    free(clip);
    clip = NULL;
    nclip = 0;
}

static void clip_fill(int move)
{
    int i, marked = count_marked();

    clip_clear();
    clip_move = move;

    if (nents == 0)
        return;

    clip = (char **)calloc(marked ? marked : 1, sizeof(char *));
    if (!clip)
        return;

    if (marked) {
        for (i = 0; i < nents; i++) {
            if (!ents[i].marked) continue;
            clip[nclip] = (char *)malloc(MAXPATH);
            if (!clip[nclip]) continue;
            path_join(clip[nclip], MAXPATH, cwd, ents[i].name);
            nclip++;
        }
    } else {
        clip[0] = (char *)malloc(MAXPATH);
        if (clip[0]) {
            path_join(clip[0], MAXPATH, cwd, ents[sel].name);
            nclip = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Render de la interfaz                                               */
/* ------------------------------------------------------------------ */

static int mode = MODE_BROWSE;
static int menu_sel = 0;

static const char *menu_items[] = {
    "Copiar", "Cortar", "Pegar aquí", "Borrar", "Nueva carpeta", "Cancelar"
};
#define MENU_N 6

static void render_base(void)
{
    char buf[512];
    int i, list_rows, first;

    fill_rect(0, 0, bufs[cur].width, bufs[cur].height, COL_BG);

    /* Cabecera */
    fill_cells(0, 0, cols, 2, COL_BAR);
    draw_text(1, 0, COL_MARK, "Gestor de archivos PS3", cols - 2);
    snprintf(buf, sizeof(buf), "%s", cwd);
    draw_text(1, 1, COL_DIR, buf, cols - 2);

    /* Lista */
    list_rows = rows - 5;
    if (list_rows < 1) list_rows = 1;

    if (sel < top) top = sel;
    if (sel >= top + list_rows) top = sel - list_rows + 1;
    if (top < 0) top = 0;

    if (nents == 0)
        draw_text(2, 3, COL_DIM, "(carpeta vacía)", cols - 4);

    first = top;
    for (i = 0; i < list_rows && first + i < nents; i++) {
        Entry *e = &ents[first + i];
        int y = 2 + i;
        uint32_t col = e->is_dir ? COL_DIR : COL_TEXT;
        char sz[32] = "";

        if (first + i == sel)
            fill_cells(0, y, cols, 1, COL_SEL);

        if (e->marked)
            draw_text(1, y, COL_MARK, "*", 1);

        if (!e->is_dir)
            fmt_size(e->size, sz, sizeof(sz));
        else
            snprintf(sz, sizeof(sz), "<DIR>");

        draw_text(3, y, e->marked ? COL_MARK : col, e->name, cols - 3 - 14);
        draw_text_right(y, COL_DIM, sz, cols - 1);
    }

    /* Pie */
    fill_cells(0, rows - 3, cols, 3, COL_BAR);
    snprintf(buf, sizeof(buf), "%d elementos   %d marcados   portapapeles: %d%s",
             nents, count_marked(), nclip, nclip ? (clip_move ? " (cortar)" : " (copiar)") : "");
    draw_text(1, rows - 3, COL_DIM, buf, cols - 2);
    draw_text(1, rows - 2, COL_TEXT,
              "X abrir   O atrás   □ marcar   △ menú   SELECT todo   START salir", cols - 2);
    if (status_msg[0])
        draw_text(1, rows - 1, status_col, status_msg, cols - 2);
}

static void render_menu(void)
{
    int w = 24, h = MENU_N + 4, x = (cols - w) / 2, y = (rows - h) / 2, i;
    draw_box(x, y, w, h);
    draw_text(x + 2, y + 1, COL_MARK, "Menú", w - 4);
    for (i = 0; i < MENU_N; i++) {
        if (i == menu_sel)
            fill_rect((x + 1) * CW + 3, (y + 2 + i) * CH, (w - 2) * CW - 6, CH, COL_SEL);
        draw_text(x + 2, y + 2 + i, COL_TEXT, menu_items[i], w - 4);
    }
}

static void render_confirm(void)
{
    char buf[128];
    int w = 40, h = 7, x = (cols - w) / 2, y = (rows - h) / 2;
    int n = count_marked() ? count_marked() : 1;
    draw_box(x, y, w, h);
    draw_text(x + 2, y + 1, COL_ERR, "¿Borrar definitivamente?", w - 4);
    snprintf(buf, sizeof(buf), "%d elemento(s). No se puede deshacer.", n);
    draw_text(x + 2, y + 3, COL_TEXT, buf, w - 4);
    draw_text(x + 2, y + 5, COL_DIM, "X = sí      O = no", w - 4);
}

static void present(void)
{
    flip_buffer(bufs[cur].id);
    cur ^= 1;
}

static void render_frame(void)
{
    render_base();
    if (mode == MODE_MENU)
        render_menu();
    else if (mode == MODE_CONFIRM_DEL)
        render_confirm();
    present();
}

/* ------------------------------------------------------------------ */
/* Progreso de operaciones largas                                      */
/* ------------------------------------------------------------------ */

static volatile int cancel_op = 0;
static char   prog_title[64];
static char   prog_file[MAXPATH];
static u64l   prog_bytes = 0;      /* bytes copiados del elemento actual */

static void draw_progress_box(void)
{
    char buf[128], sz[32];
    int w = cols - 8, h = 7, x = 4, y = (rows - h) / 2;
    if (w > 70) { w = 70; x = (cols - w) / 2; }
    draw_box(x, y, w, h);
    draw_text(x + 2, y + 1, COL_MARK, prog_title, w - 4);
    draw_text(x + 2, y + 3, COL_TEXT, prog_file, w - 4);
    fmt_size(prog_bytes, sz, sizeof(sz));
    snprintf(buf, sizeof(buf), "Copiado: %s", sz);
    draw_text(x + 2, y + 4, COL_DIM, buf, w - 4);
    draw_text(x + 2, y + 5, COL_DIM, "Mantén O para cancelar", w - 4);
}

/* Prepara ambos buffers con la interfaz base y luego solo se redibuja
 * el cuadro de progreso (rapido incluso en 1080p). */
static void progress_begin(const char *title)
{
    snprintf(prog_title, sizeof(prog_title), "%s", title);
    prog_file[0] = 0;
    prog_bytes = 0;
    cancel_op = 0;

    render_base();
    draw_progress_box();
    present();
    render_base();
    draw_progress_box();
    present();
}

static void progress_update(void)
{
    sysUtilCheckCallback();
    if (!running)
        cancel_op = 1;
    if (read_pad() & BTN_CIRCLE_M)
        cancel_op = 1;
    draw_progress_box();
    present();
}

/* ------------------------------------------------------------------ */
/* Operaciones de archivos                                             */
/* ------------------------------------------------------------------ */

static unsigned char *copy_buf = NULL;
static int op_errors = 0;

static int copy_file(const char *src, const char *dst)
{
    int in, out;
    ssize_t r;
    int ok = 1;
    u64l since_update = 0;

    in = open(src, O_RDONLY);
    if (in < 0) { op_errors++; return 0; }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { close(in); op_errors++; return 0; }

    snprintf(prog_file, sizeof(prog_file), "%s", base_name(src));
    prog_bytes = 0;
    progress_update();

    while ((r = read(in, copy_buf, COPY_BUF)) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, copy_buf + off, r - off);
            if (w <= 0) { ok = 0; break; }
            off += w;
        }
        if (!ok) break;
        prog_bytes += (u64l)r;
        since_update += (u64l)r;
        if (since_update >= (4ULL * 1024 * 1024)) {
            since_update = 0;
            progress_update();
        }
        if (cancel_op) { ok = 0; break; }
    }
    if (r < 0) ok = 0;

    close(in);
    close(out);
    if (!ok) {
        unlink(dst);   /* no dejar archivos a medias */
        op_errors++;
    }
    return ok;
}

static int copy_recursive(const char *src, const char *dst)
{
    struct stat st;

    if (cancel_op)
        return 0;
    if (stat(src, &st) != 0) { op_errors++; return 0; }

    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;
        int ok = 1;

        if (mkdir(dst, 0777) != 0 && errno != EEXIST) { op_errors++; return 0; }
        d = opendir(src);
        if (!d) { op_errors++; return 0; }
        while ((de = readdir(d)) != NULL && !cancel_op) {
            char s2[MAXPATH], d2[MAXPATH];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            path_join(s2, sizeof(s2), src, de->d_name);
            path_join(d2, sizeof(d2), dst, de->d_name);
            if (!copy_recursive(s2, d2))
                ok = 0;
        }
        closedir(d);
        return ok && !cancel_op;
    }
    return copy_file(src, dst);
}

static int delete_recursive(const char *path)
{
    struct stat st;

    if (cancel_op)
        return 0;
    if (stat(path, &st) != 0) { op_errors++; return 0; }

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        int ok = 1;
        if (!d) { op_errors++; return 0; }
        while ((de = readdir(d)) != NULL && !cancel_op) {
            char child[MAXPATH];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            path_join(child, sizeof(child), path, de->d_name);
            if (!delete_recursive(child))
                ok = 0;
        }
        closedir(d);
        if (!ok || cancel_op)
            return 0;
        if (rmdir(path) != 0) { op_errors++; return 0; }
        return 1;
    }

    snprintf(prog_file, sizeof(prog_file), "%s", base_name(path));
    progress_update();
    if (unlink(path) != 0) { op_errors++; return 0; }
    return 1;
}

/* Si dst existe, genera un nombre libre: "nombre (2).ext" */
static void make_unique(char *dst, size_t n)
{
    char stem[MAXPATH], ext[64] = "";
    char *dot, *slash;
    int i;

    if (!path_exists(dst))
        return;

    snprintf(stem, sizeof(stem), "%s", dst);
    slash = strrchr(stem, '/');
    dot = strrchr(stem, '.');
    if (dot && slash && dot > slash + 1) {
        snprintf(ext, sizeof(ext), "%s", dot);
        *dot = 0;
    }
    for (i = 2; i < 10000; i++) {
        snprintf(dst, n, "%s (%d)%s", stem, i, ext);
        if (!path_exists(dst))
            return;
    }
}

static int is_inside(const char *path, const char *parent)
{
    size_t l = strlen(parent);
    return strncmp(path, parent, l) == 0 && (path[l] == '/' || path[l] == 0);
}

static void do_paste(void)
{
    int i, done = 0;

    if (nclip == 0) { set_status(COL_ERR, "El portapapeles está vacío."); return; }
    if (strcmp(cwd, "/") == 0) { set_status(COL_ERR, "No se puede pegar en la raíz."); return; }
    if (is_protected(cwd)) { set_status(COL_ERR, "Carpeta de solo lectura / sistema: bloqueada."); return; }

    copy_buf = (unsigned char *)malloc(COPY_BUF);
    if (!copy_buf) { set_status(COL_ERR, "Sin memoria."); return; }

    op_errors = 0;
    progress_begin(clip_move ? "Moviendo..." : "Copiando...");

    for (i = 0; i < nclip && !cancel_op; i++) {
        char dst[MAXPATH];
        const char *src = clip[i];

        path_join(dst, sizeof(dst), cwd, base_name(src));

        if (clip_move && is_protected(src)) { op_errors++; continue; }
        if (is_directory(src) && is_inside(cwd, src)) { op_errors++; continue; }

        if (clip_move) {
            char srcdir[MAXPATH];
            char *sl;
            snprintf(srcdir, sizeof(srcdir), "%s", src);
            sl = strrchr(srcdir, '/');
            if (sl == srcdir) srcdir[1] = 0; else if (sl) *sl = 0;
            if (!strcmp(srcdir, cwd))
                continue;    /* mismo sitio: nada que mover */
        }

        make_unique(dst, sizeof(dst));
        snprintf(prog_file, sizeof(prog_file), "%s", base_name(src));

        if (clip_move) {
            if (rename(src, dst) == 0) {
                done++;
            } else if (copy_recursive(src, dst)) {
                /* Entre dispositivos: copiado, ahora borrar el origen */
                if (delete_recursive(src))
                    done++;
                else
                    op_errors++;
            }
        } else {
            if (copy_recursive(src, dst))
                done++;
        }
    }

    free(copy_buf);
    copy_buf = NULL;

    if (clip_move && done > 0)
        clip_clear();

    if (cancel_op)
        set_status(COL_ERR, "Cancelado. Hechos: %d.", done);
    else if (op_errors)
        set_status(COL_ERR, "Terminado con %d error(es). Hechos: %d.", op_errors, done);
    else
        set_status(COL_OK, "%s %d elemento(s).", clip_move ? "Movidos" : "Copiados", done);

    load_dir();
}

static void do_delete(void)
{
    int i, done = 0, marked = count_marked();

    if (nents == 0) return;
    if (is_protected(cwd) || strcmp(cwd, "/") == 0) {
        set_status(COL_ERR, "Carpeta de solo lectura / sistema: bloqueada.");
        return;
    }

    op_errors = 0;
    progress_begin("Borrando...");

    for (i = 0; i < nents && !cancel_op; i++) {
        char p[MAXPATH];
        if (marked ? !ents[i].marked : i != sel)
            continue;
        path_join(p, sizeof(p), cwd, ents[i].name);
        if (delete_recursive(p))
            done++;
    }

    if (cancel_op)
        set_status(COL_ERR, "Cancelado. Borrados: %d.", done);
    else if (op_errors)
        set_status(COL_ERR, "Terminado con %d error(es). Borrados: %d.", op_errors, done);
    else
        set_status(COL_OK, "Borrados %d elemento(s).", done);

    load_dir();
}

static void do_mkdir(void)
{
    char p[MAXPATH];
    int i;

    if (strcmp(cwd, "/") == 0 || is_protected(cwd)) {
        set_status(COL_ERR, "Carpeta de solo lectura / sistema: bloqueada.");
        return;
    }
    path_join(p, sizeof(p), cwd, "Nueva carpeta");
    for (i = 2; path_exists(p) && i < 1000; i++) {
        char name[64];
        snprintf(name, sizeof(name), "Nueva carpeta %d", i);
        path_join(p, sizeof(p), cwd, name);
    }
    if (mkdir(p, 0777) == 0)
        set_status(COL_OK, "Creada: %s", base_name(p));
    else
        set_status(COL_ERR, "No se pudo crear la carpeta (errno %d).", errno);
    load_dir();
}

/* ------------------------------------------------------------------ */
/* Bucle principal                                                     */
/* ------------------------------------------------------------------ */

static void run_menu_action(int item)
{
    switch (item) {
    case 0:
        clip_fill(0);
        set_status(COL_OK, "%d elemento(s) copiados al portapapeles.", nclip);
        break;
    case 1:
        clip_fill(1);
        set_status(COL_OK, "%d elemento(s) listos para mover.", nclip);
        break;
    case 2:
        mode = MODE_BROWSE;
        do_paste();
        return;
    case 3:
        if (nents > 0) {
            mode = MODE_CONFIRM_DEL;
            return;
        }
        break;
    case 4:
        mode = MODE_BROWSE;
        do_mkdir();
        return;
    default:
        break;
    }
    mode = MODE_BROWSE;
}

int main(int argc, char *argv[])
{
    unsigned prev = 0, held_frames = 0;
    int dirty = 1;

    (void)argc; (void)argv;

    sysUtilRegisterCallback(0, sysutil_cb, NULL);
    ioPadInit(7);
    init_video();

    if (!is_directory(cwd))
        snprintf(cwd, sizeof(cwd), "/");
    load_dir();

    while (running) {
        unsigned now, pressed, rep;
        int list_rows = rows - 5;
        if (list_rows < 1) list_rows = 1;

        sysUtilCheckCallback();
        now = read_pad();
        pressed = now & ~prev;

        /* Autorepeticion para arriba/abajo */
        rep = 0;
        if (now & (BTN_UP_M | BTN_DOWN_M)) {
            held_frames++;
            if (held_frames > 20 && (held_frames % 3) == 0)
                rep = now & (BTN_UP_M | BTN_DOWN_M);
        } else {
            held_frames = 0;
        }
        pressed |= rep;

        if (pressed) {
            dirty = 1;
            if (mode == MODE_BROWSE) {
                if (pressed & BTN_UP_M)   { if (sel > 0) sel--; }
                if (pressed & BTN_DOWN_M) { if (sel < nents - 1) sel++; }
                if (pressed & BTN_L1_M)   { sel -= list_rows; if (sel < 0) sel = 0; }
                if (pressed & BTN_R1_M)   { sel += list_rows; if (sel > nents - 1) sel = nents - 1; if (sel < 0) sel = 0; }

                if (pressed & BTN_CROSS_M) {
                    if (nents > 0 && ents[sel].is_dir) {
                        char p[MAXPATH];
                        path_join(p, sizeof(p), cwd, ents[sel].name);
                        enter_dir(p);
                    } else if (nents > 0) {
                        char sz[32];
                        fmt_size(ents[sel].size, sz, sizeof(sz));
                        set_status(COL_DIM, "%s  (%s)", ents[sel].name, sz);
                    }
                }
                if (pressed & BTN_CIRCLE_M)
                    go_up();
                if ((pressed & BTN_SQUARE_M) && nents > 0) {
                    ents[sel].marked ^= 1;
                    if (sel < nents - 1) sel++;
                }
                if (pressed & BTN_SELECT_M) {
                    int i, all = (count_marked() == nents);
                    for (i = 0; i < nents; i++)
                        ents[i].marked = !all;
                }
                if (pressed & BTN_TRI_M) {
                    mode = MODE_MENU;
                    menu_sel = 0;
                }
                if (pressed & BTN_START_M)
                    running = 0;
            } else if (mode == MODE_MENU) {
                if (pressed & BTN_UP_M)   menu_sel = (menu_sel + MENU_N - 1) % MENU_N;
                if (pressed & BTN_DOWN_M) menu_sel = (menu_sel + 1) % MENU_N;
                if (pressed & (BTN_CIRCLE_M | BTN_TRI_M)) mode = MODE_BROWSE;
                if (pressed & BTN_CROSS_M) run_menu_action(menu_sel);
            } else if (mode == MODE_CONFIRM_DEL) {
                if (pressed & BTN_CROSS_M) {
                    mode = MODE_BROWSE;
                    do_delete();
                } else if (pressed & BTN_CIRCLE_M) {
                    mode = MODE_BROWSE;
                }
            }
        }
        prev = now;

        if (dirty) {
            render_frame();
            dirty = 0;
        }
        usleep(16000);
    }

    clip_clear();
    free(ents);
    gcmSetWaitFlip(context);
    return 0;
}
