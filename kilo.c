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
#include <fcntl.h>

/*** defines ***/
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f) // Ctrl + [A-Z] map to bytes 1-26

enum editorKey {
    BACKSPACE = 127,
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
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsgTime;
    struct termios origTermios;
};
struct editorConfig EditorConfig;

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt);

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
    for (int i = 0; i < cx; i++) {
        if (row->chars[i] == '\t') rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }

    return rx;
}

void editorUpdateRow(editorRow *row) {
    int tabs = 0;
    for (int i = 0; i < row->size; i++)
        if (row->chars[i] == '\t') tabs++;
    
    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int i = 0; i < row->size; i++) {
        if (row->chars[i] == '\t') {
            row->render[idx++] = ' ';
            while (idx % 8 != 0) row->render[idx++] = ' ';
        } else row->render[idx++] = row->chars[i];
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int pos, char *s, size_t len) {
    if (pos < 0 || pos > EditorConfig.numrows) return;

    EditorConfig.row = realloc(EditorConfig.row, sizeof(editorRow) * (EditorConfig.numrows + 1));
    memmove(&EditorConfig.row[pos + 1], &EditorConfig.row[pos], sizeof(editorRow) * (EditorConfig.numrows - pos));

    EditorConfig.row[pos].size = len;
    EditorConfig.row[pos].chars = malloc(len + 1);
    memcpy(EditorConfig.row[pos].chars, s, len);
    EditorConfig.row[pos].chars[len] = '\0';

    EditorConfig.row[pos].rsize = 0;
    EditorConfig.row[pos].render = NULL;
    editorUpdateRow(&EditorConfig.row[pos]);

    EditorConfig.numrows++;
    EditorConfig.dirty++;
}

void editorFreeRow(editorRow *row) {
    free(row->render);
    free(row->chars);
}

void editorDeleteRow(int pos) {
    if (pos < 0 || pos >= EditorConfig.numrows) return;
    editorFreeRow(&EditorConfig.row[pos]);
    memmove(&EditorConfig.row[pos], &EditorConfig.row[pos + 1], sizeof(editorRow) * (EditorConfig.numrows - pos - 1));
    EditorConfig.numrows--;
    EditorConfig.dirty++;
}

void editorRowInsertChar(editorRow *row, int pos, int c) {
    if (pos < 0 || pos > row->size) pos = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[pos + 1], &row->chars[pos], row->size - pos + 1);
    row->size++;
    row->chars[pos] = c;
    editorUpdateRow(row);
}

void editorRowAppendString(editorRow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    EditorConfig.dirty++;
}

void editorRowDeleteChar(editorRow *row, int pos) {
    if (pos < 0 || pos >= row->size) return;
    memmove(&row->chars[pos], &row->chars[pos + 1], row->size - pos);
    row->size--;
    editorUpdateRow(row);
    EditorConfig.dirty++;
}

/*** editor operations ***/
void editorInsertChar(int c) {
    if (EditorConfig.cy == EditorConfig.numrows) editorInsertRow(EditorConfig.numrows, "", 0);
    editorRowInsertChar(&EditorConfig.row[EditorConfig.cy], EditorConfig.cx, c);
    EditorConfig.cx++;
}

void editorInsertNewLine(void) {
    if (EditorConfig.cx == 0) editorInsertRow(EditorConfig.cy, "", 0);
    else {
        editorRow *row = &EditorConfig.row[EditorConfig.cy];
        editorInsertRow(EditorConfig.cy + 1, &row->chars[EditorConfig.cx], row->size - EditorConfig.cx);
        row = &EditorConfig.row[EditorConfig.cy];
        row->size = EditorConfig.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    EditorConfig.cy++;
    EditorConfig.cx = 0;
}

void editorDeleteChar(void) {
    if (EditorConfig.cy == EditorConfig.numrows) return;
    if (EditorConfig.cx == 0 && EditorConfig.cy == 0) return;

    editorRow *row = &EditorConfig.row[EditorConfig.cy];
    if (EditorConfig.cx > 0) {
        editorRowDeleteChar(row, EditorConfig.cx - 1);
        EditorConfig.cx--;
    } else {
        EditorConfig.cx = EditorConfig.row[EditorConfig.cy - 1].size;
        editorRowAppendString(&EditorConfig.row[EditorConfig.cy - 1], row->chars, row->size);
        editorDeleteRow(EditorConfig.cy);
        EditorConfig.cy--;
    }
}

/*** file i/o ***/
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int i = 0; i < EditorConfig.numrows; i++)
        totlen += EditorConfig.row[i].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (int i = 0; i < EditorConfig.numrows; i++) {
        memcpy(p, EditorConfig.row[i].chars, EditorConfig.row[i].size);
        p += EditorConfig.row[i].size;
        *p = '\n';
        p++;
    }

    return buf;
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
        editorInsertRow(EditorConfig.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    EditorConfig.dirty = 0;
}

void editorSave(void) {
    if (EditorConfig.filename == NULL){
        EditorConfig.filename = editorPrompt("Save as: %s");
        if (EditorConfig.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(EditorConfig.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                EditorConfig.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
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
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                        EditorConfig.filename ? EditorConfig.filename : "[No Name]",
                        EditorConfig.numrows,
                        EditorConfig.dirty ? "(modified)" : "");
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
char *editorPrompt(char *prompt) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

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
    static int quit_times = KILO_QUIT_TIMES;
    
    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewLine();
            break;

        case CTRL_KEY('q'):
            if (EditorConfig.dirty > 0 && quit_times > 0) {
                editorSetStatusMessage("WARNING! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            EditorConfig.cx = 0;
            break;

        case END_KEY:
            if (EditorConfig.cy < EditorConfig.numrows) EditorConfig.cx = EditorConfig.row[EditorConfig.cy].size;
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDeleteChar();
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

        case CTRL_KEY('l'):
        case '\x1b':
            break;
        
        default:
            editorInsertChar(c);
            break;
    }

    quit_times = KILO_QUIT_TIMES;
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
    EditorConfig.dirty = 0;
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

    editorSetStatusMessage("HELP: Ctrl-Q = quit | Ctrl-S = save");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
