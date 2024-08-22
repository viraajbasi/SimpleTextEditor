/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

/*** defines ***/
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define CTRL_KEY(k) ((k) & 0x1f) // Ctrl + [A-Z] map to bytes 1-26

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/
typedef struct editorRow {
    int size;
    int rsize;
    char *chars;
    char *render;
} editorRow;

struct editorConfig {
    int cx;
    int cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    editorRow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsgTime;
    struct termios origTermios;
};
struct editorConfig EditorConfig;

/*** terminal ***/
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &EditorConfig.origTermios) == -1) die("tcsetattr");
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &EditorConfig.origTermios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    
    struct termios raw = EditorConfig.origTermios;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey(void) {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
        if (nread == 1 && errno != EAGAIN) die("read");

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'F': return END_KEY;
                    case 'H': return HOME_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'F': return END_KEY;
                case 'H': return HOME_KEY;
            }
        }

        return '\x1b';
    } else return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;

        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        
        return 0;
    }
}

/*** row operations ***/
int editorRowCxToRx(editorRow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }

    return rx;
}

void editorUpdateRow(editorRow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;
    
    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % 8 != 0) row->render[idx++] = ' ';
        } else row->render[idx++] = row->chars[j];
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
    EditorConfig.row = realloc(EditorConfig.row, sizeof(editorRow) * (EditorConfig.numrows + 1));

    int at = EditorConfig.numrows;
    EditorConfig.row[at].size = len;
    EditorConfig.row[at].chars = malloc(len + 1);
    memcpy(EditorConfig.row[at].chars, s, len);
    EditorConfig.row[at].chars[len] = '\0';

    EditorConfig.row[at].rsize = 0;
    EditorConfig.row[at].render = NULL;
    editorUpdateRow(&EditorConfig.row[at]);

    EditorConfig.numrows++;
}

void editorOpen(char *filename) {
    free(EditorConfig.filename);
    EditorConfig.filename = strdup(filename);
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
}

/*** append buffer ***/
struct AppendBuffer {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct AppendBuffer *appendBuffer, const char *s, int len) {
    char *new = realloc(appendBuffer->b, appendBuffer->len + len);

    if (new == NULL) return;
    memcpy(&new[appendBuffer->len], s, len);
    appendBuffer->b = new;
    appendBuffer->len += len;
}

void abFree(struct AppendBuffer *appendBuffer) {
    free(appendBuffer->b);
}

/*** output ***/
void editorScroll(void) {
    EditorConfig.rx = 0;
    if (EditorConfig.cy < EditorConfig.numrows) EditorConfig.rx = editorRowCxToRx(&EditorConfig.row[EditorConfig.cy], EditorConfig.cx);
    if (EditorConfig.cy < EditorConfig.rowoff) EditorConfig.rowoff = EditorConfig.cy;
    if (EditorConfig.cy >= EditorConfig.rowoff + EditorConfig.screenrows) EditorConfig.rowoff = EditorConfig.cy - EditorConfig.screenrows + 1;
    if (EditorConfig.rx < EditorConfig.coloff) EditorConfig.coloff = EditorConfig.rx;
    if (EditorConfig.rx >= EditorConfig.coloff + EditorConfig.screencols) EditorConfig.coloff = EditorConfig.rx - EditorConfig.screencols + 1;
}

void editorDrawRows(struct AppendBuffer *appendBuffer) {
    int y;
    for (y = 0; y < EditorConfig.screenrows; y++) {
        int filerow = y + EditorConfig.rowoff;
        if (filerow >= EditorConfig.numrows) {
            if (EditorConfig.numrows == 0 && y == EditorConfig.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
                if (welcomelen > EditorConfig.screencols) welcomelen = EditorConfig.screencols;
                int padding = (EditorConfig.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(appendBuffer, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(appendBuffer, " ", 1);
                abAppend(appendBuffer, welcome, welcomelen);
            } else abAppend(appendBuffer, "~", 1);
        } else {
            int len = EditorConfig.row[filerow].rsize - EditorConfig.coloff;
            if (len < 0) len = 0;
            if (len > EditorConfig.screencols) len = EditorConfig.screencols;
            abAppend(appendBuffer, &EditorConfig.row[filerow].render[EditorConfig.coloff], len);
        }

        abAppend(appendBuffer, "\x1b[K", 3);
        abAppend(appendBuffer, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct AppendBuffer *appendBuffer) {
    abAppend(appendBuffer, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines", EditorConfig.filename ? EditorConfig.filename : "[No Name]", EditorConfig.numrows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", EditorConfig.cy + 1, EditorConfig.numrows);
    if (len > EditorConfig.screencols) len = EditorConfig.screencols;
    abAppend(appendBuffer, status, len);
    while (len < EditorConfig.screencols) {
        if (EditorConfig.screencols - len == rlen) {
            abAppend(appendBuffer, rstatus, rlen);
            break;
        } else {
            abAppend(appendBuffer, " ", 1);
            len++;
        }
    }
    abAppend(appendBuffer, "\x1b[m", 3);
    abAppend(appendBuffer, "\r\n", 2);
}

void editorDrawMessageBar(struct AppendBuffer *appendBuffer) {
    abAppend(appendBuffer, "\x1b[K", 3);
    int msglen = strlen(EditorConfig.statusmsg);
    if (msglen > EditorConfig.screencols) msglen = EditorConfig.screencols;
    if (msglen && time(NULL) - EditorConfig.statusmsgTime < 5) abAppend(appendBuffer, EditorConfig.statusmsg, msglen);
}

void editorRefreshScreen(void) {
    editorScroll();

    struct AppendBuffer appendBuffer = ABUF_INIT;

    abAppend(&appendBuffer, "\x1b[?25l", 6);
    abAppend(&appendBuffer, "\x1b[H", 3);

    editorDrawRows(&appendBuffer);
    editorDrawStatusBar(&appendBuffer);
    editorDrawMessageBar(&appendBuffer);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (EditorConfig.cy - EditorConfig.rowoff) + 1, (EditorConfig.rx - EditorConfig.coloff) + 1);
    abAppend(&appendBuffer, buf, strlen(buf));

    abAppend(&appendBuffer, "\x1b[?25h", 6);

    write(STDOUT_FILENO, appendBuffer.b, appendBuffer.len);
    abFree(&appendBuffer);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(EditorConfig.statusmsg, sizeof(EditorConfig.statusmsg), fmt, ap);
    va_end(ap);
    EditorConfig.statusmsgTime = time(NULL);
}

/*** input ***/
void editorMoveCursor(int key) {
    editorRow *row = (EditorConfig.cy >= EditorConfig.numrows) ? NULL : &EditorConfig.row[EditorConfig.cy];

    switch (key) {
        case ARROW_LEFT:
            if (EditorConfig.cx != 0) EditorConfig.cx--;
            else if (EditorConfig.cy > 0) {
                EditorConfig.cy--;
                EditorConfig.cx = EditorConfig.row[EditorConfig.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && EditorConfig.cx < row->size) EditorConfig.cx++;
            else if (row && EditorConfig.cx == row->size) {
                EditorConfig.cy++;
                EditorConfig.cx = 0;
            }
            break;
        case ARROW_UP:
            if (EditorConfig.cy != 0) EditorConfig.cy--;
            break;
        case ARROW_DOWN:
            if (EditorConfig.cy < EditorConfig.numrows) EditorConfig.cy++;
            break;
    }

    row = (EditorConfig.cy >= EditorConfig.numrows) ? NULL : &EditorConfig.row[EditorConfig.cy];
    int rowlen = row ? row->size : 0;
    if (EditorConfig.cx > rowlen)
        EditorConfig.cx = rowlen;
}

void editorProcessKeypress(void) {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            EditorConfig.cx = 0;
            break;

        case END_KEY:
            if (EditorConfig.cy < EditorConfig.numrows) EditorConfig.cx = EditorConfig.row[EditorConfig.cy].size;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP)
                    EditorConfig.cy = EditorConfig.rowoff;
                else if (c == PAGE_DOWN) {
                    EditorConfig.cy = EditorConfig.rowoff + EditorConfig.screenrows - 1;
                    if (EditorConfig.cy > EditorConfig.numrows) EditorConfig.cy = EditorConfig.numrows;
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*** init ***/
void initEditor(void) {
    EditorConfig.cx = 0;
    EditorConfig.cy = 0;
    EditorConfig.rx = 0;
    EditorConfig.rowoff = 0;
    EditorConfig.coloff = 0;
    EditorConfig.numrows = 0;
    EditorConfig.row = NULL;
    EditorConfig.filename = NULL;
    EditorConfig.statusmsg[0] = '\0';
    EditorConfig.statusmsgTime = 0;

    if (getWindowSize(&EditorConfig.screenrows, &EditorConfig.screencols) == -1) die("getWindowSize");
    EditorConfig.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    
    if (argc >= 2) editorOpen(argv[1]);

    editorSetStatusMessage("HELP: Ctrl-Q = quit");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
