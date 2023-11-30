#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>


/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define VERSION "0.0.1"
#define ABUF_INIT {NULL, 0}


/*** data ***/
struct editorConfig 
{
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

// append buffer
struct abuf 
{
    char *b;
    int len;
};

struct editorConfig E;


/*** function prototypes ***/
void enableRawMode();
void disableRawMode();
void die(const char* s);
void editorProcessKeypress();
void editorRefreshScreen();
int getWindowSize(int* rows, int* cols);
void initEditor();
char editorReadKey();


/*** main ***/
int main() 
{
    enableRawMode();
    initEditor();

    while (1) 
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}

void initEditor()
{
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}


/*** terminal ***/
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

// error handling (print out error if function returns -1)
void die(const char* s) 
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3); // position the cursor

    perror(s);
    exit(1);
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

// wait for 1 keypress, then return it. deals with low-level terminal input
char editorReadKey()
{
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) 
    {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    return c;
}

void abAppend(struct abuf* ab, const char* s, int len) 
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


/*** input ***/
// wait for keypress, then handle it. deals with mapping keys to editor functions at a much higher level
void editorProcessKeypress() 
{
    char c = editorReadKey();

    switch (c) 
    {
        case CTRL_KEY('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    }
}


/*** output ***/
// handle drawing each row of buffer of text being edited
void editorDrawRows(struct abuf* ab) 
{
    int y;

    for (y = 0; y < E.screenrows; y++) 
    {
        if (y == E.screenrows / 3) 
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
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}
