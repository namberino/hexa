#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define VERSION "0.0.1"
#define ABUF_INIT {NULL, 0}

// 1000: out of range of char so they don't conflict with normal keypress
enum editorKey 
{
    ARROW_LEFT = 1000,
    ARROW_RIGHT = 1001,
    ARROW_UP = 1002,
    ARROW_DOWN = 1003,
    HOME_KEY = 1004,
    END_KEY = 1005,
    PAGE_UP = 1006,
    PAGE_DOWN = 1007,
    DEL_KEY = 1008
};


/*** data ***/
// editor row (for storage)
typedef struct erow 
{
    int size;
    char* chars;
} erow;

// append buffer
struct abuf 
{
    char* b;
    int len;
};
struct editorConfig 
{
    int cx, cy;
    int rowoff; // vertical scroll offset
    int coloff; // horizontal scroll offset
    int screenrows;
    int screencols;
    int numrows;
    erow* row; // array for storing multiple lines
    struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/
// error handling (print out error if function returns -1)
void die(const char* s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3); // position the cursor

    perror(s);
    exit(1);
}

void disableRawMode() 
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() 
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode); // called automatically when program exits

    struct termios raw = E.orig_termios;

    // ICANON turns off canonical mode (allow reading input byte by byte instead of line by line)
	// ISIG turns off signals from Ctrl-C and Ctrl-Z
	// IXON turns off signals from Ctrl-Q and Ctrl-S
	// IEXTEN turns off signals from Ctrl-V and Ctrl-O
	// ICRNL turns off Ctrl-M (prevent tranlation of carriage 13 - '\r' into newlines 10 - '\n')
	// OPOST turns off the translation of '\n' to "\r\n" (carriage return)
	// When BRKINT is on, a break condition will cause a SIGINT signal to be sent to the program
	// INPCK enables parity checking
	// ISTRIP causes the 8th bit of each input byte to be stripped (set to 0)
	// CS8 is a bit mask with multiple bits (sets character size CS to 8 bits per byte)
	// Turn off ECHO to prevent letters being printed to terminal, similar to typing password when using sudo
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0; // sets the minimum number of bytes of input needed before read() can return
    raw.c_cc[VTIME] = 1; // sets the maximum amount of time to wait before read() returns

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// wait for 1 keypress, then return it. deals with low-level terminal input
int editorReadKey()
{
    int nread;
    char c;
    
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) 
    {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        
        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9') 
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';

                if (seq[2] == '~') 
                {
                    // reading the escape sequence
                    switch (seq[1]) 
                    {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }
    else
    {
        return c;
    }
}

int getCursorPosition(int* rows, int* cols) 
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) 
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

// getting window size
int getWindowSize(int* rows, int* cols) 
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1; // fallback method for when ioctl doesn't work

        return getCursorPosition(rows, cols);
    } 
    else
    { 
        *cols = ws.ws_col;
        *rows = ws.ws_row;

        return 0;
    }
}

void abAppend(struct abuf *ab, const char *s, int len) 
{
    char* new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf* ab) 
{
    free(ab->b);
}

/*** row operations ***/
void editorAppendRow(char* s, size_t len)
{
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows; // set to index of new row to initialize

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);

    memcpy(E.row[at].chars, s, len);

    E.row[at].chars[len] = '\0';
    E.numrows++;
}


/*** file IO ***/
// for opening and reading file from disk
void editorOpen(char* filename)
{
    FILE* fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    // getline return -1 when it gets to the end of the file (as there's no more line to read)
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;

        editorAppendRow(line, linelen);
    }

    free(line);
    fclose(fp);
}


/*** output ***/
// check if cursor moved outside of screen, if so, adjust E.rowoff so that cursor is inside visible window
void editorScroll() {
    // check if cursor is above the visible window
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;

    // check if cursor is past the bottom of the visible window
    if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows + 1; // since E.rowoff refers to top of the screen

    // check if cursor is to the left of the visible window
    if (E.cx < E.coloff)
        E.coloff = E.cx;

    // check if cursor is to the right of the visible window
    if (E.cx >= E.coloff + E.screencols)
        E.coloff = E.cx - E.screencols + 1;
}

// handle drawing each row of buffer of text being edited
void editorDrawRows(struct abuf* ab)
{
    int y;

    for (y = 0; y < E.screenrows; y++) 
    {
        int filerow = y + E.rowoff; // for displaying the row of the file at y position

        if (filerow >= E.numrows)
        {
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Unnamed Editor - Version %s", VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;

                int padding = (E.screencols - welcomelen) / 2;

                if (padding) 
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);
            }
            else 
            {
                abAppend(ab, "~", 1);
            }
        }
        else 
        {
            // subtract the number of characters that are to the left of the offset from the length of the row
            int len = E.row[filerow].size - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].chars[E.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3);

        if (y < E.screenrows - 1) 
        {
            abAppend(ab, "\r\n", 2);
        }
    }
}

// writing an escape sequence to the terminal
void editorRefreshScreen() 
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + 1); // reposition the cursor by subtracting rowoff with cy and coloff with cx
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/
void editorMoveCursor(int key)
{
    // check if cursor is on the line
    // If it is, then the row variable will point to the erow that the cursor is on, 
    // and we’ll check whether E.cx is to the left of the end of that line before we allow the cursor to move to the right
    erow* row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy]; // for limiting scrolling past the end of the current line

    switch (key) 
    {
        case ARROW_LEFT:
            if (E.cx != 0)
                E.cx--;
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) // limiting scrolling past the end of the current line
                E.cx++;
            break;
        case ARROW_UP:
            if (E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows)
                E.cy++;
            break;
    }

    // set row again, since E.cy could point to a different line than it did before
    // set E.cx to the end of that line if E.cx is to the right of the end of that line
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;

    if (E.cx > rowlen)
        E.cx = rowlen;
}

// wait for keypress, then handle it. deals with mapping keys to editor functions at a much higher level
void editorProcessKeypress() 
{
    int c = editorReadKey();

    switch (c) 
    {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screencols - 1;
            break;

        // move to either the beginning or the end of the screen
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screenrows; // how many times to move

                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
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

/*** main ***/
void initEditor() 
{
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char* argv[]) 
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
        editorOpen(argv[1]);

    while (1) 
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
