#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

// setting up ctrl-q to quit
// use hexidecimal because C doesn't have binary literals
// this mirrors what the ctrl key does and sets the upper 3 bits to 0
#define CTRL_KEY(k) ((k) & 0x1f)

#define ABBY_VER "0.0.1"



/* Declarations */

// the editor row
// the typedef lets me refer to the type as erow instead of struct erow
typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx;
  int cy;
  int term_rows;
  int term_cols;
  int numrows;
  erow row;
  struct termios org_term;
};

struct editorConfig E;

// the append buffer
// using this to make a kind of dynamic string
// which lets me simplify write()
struct abuf {
  char *b;
  int len;
};
#define ABUF_INIT { NULL, 0 }


// disable console echo
// this will make no input showup, like entering a password
// I'm refering to this as "raw" mode
void StartRawMode();

// this will exit raw mode
void EndRawMode();

// this kills the editor
void Yamete(const char *s);

// get keyboard input
char ReadKey();
void ProcessKey();

// screen drawing
void ScreenRefresh();
void DrawRows(struct abuf *ab);
int WindowSize(int *rows, int *cols);

// cursor stuff
int CursorPosition(int *rows, int *cols);
void MoveCursor(char key);

// start the editor
void InitEditor();

// append to the buffer
void abufAppend(struct abuf *ab, const char *s, int len);

// free up the buffer
void abufFree(struct abuf *ab);

void OnEditOpen();



/* Functions */

char ReadKey() {
  int n;
  char c;
  while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
    if (n == -1 && errno != EAGAIN) {
      Yamete("read");
    }
  }

  return c;
}

void ProcessKey() {
  char c = ReadKey();
  switch (c) {
    case CTRL_KEY('q'):
      // clear the screen on exit
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;


    case 8: // home key
      E.cx = 0;
      break;
    case 9: // end key
      E.cx = E.term_cols - 1;
      break;

    case 5: // page up
    case 6: // page down
      for (int n = E.term_rows; n > 0; --n) {
        if (c == 5) MoveCursor(65);
        else if (c == 6) MoveCursor(66);
      }
      break;


    case 65: // arrowkey up
    case 66: // arrowkey down
    case 67: // arrowkey right
    case 68: // arrowkey left
      MoveCursor(c);
      break;
  }
  return;
}

void StartRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.org_term) == -1) {
    Yamete("tcgetattr");
  }
  atexit(EndRawMode);

  struct termios raw_term = E.org_term;

  // these are bitflags
  // first some old miscellaneous flags for old terminal emulators
  // BRKINT is break checking, INPCK is parity checking, ISTRIP is 8th bit strip
  raw_term.c_iflag &= ~(BRKINT | INPCK | ISTRIP);
  // CS8 is a bitmask
  raw_term.c_cflag |= (CS8);
  // the IXON flag stops manual software flow control from the old days
  // I stops input flag, CR stops carriage return, and NL stops new line
  raw_term.c_iflag &= ~(ICRNL | IXON);
  // this one turns off output processing
  raw_term.c_oflag &= ~(OPOST);
  // the ICANON lets it read input bit by bit instead of line by line
  // the ISIG flag stops the ctrl-z and ctrl-y signals in the terminal
  // the IEXTEN flag stops ctrl-o and ctrl-v
  raw_term.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

  // I'm going to have read() timeout if it doesn't get input after a while
  raw_term.c_cc[VMIN] = 0;
  raw_term.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term) == -1) {
    Yamete("tcsetattr");
  }
  return;
}

void EndRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.org_term) == -1) {
    Yamete("tcsetattr");
  }
  return;
}

void Yamete(const char *s) {
  // clear the screen on exit
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
  return;
}

void ScreenRefresh() {
  struct abuf ab = ABUF_INIT;

  // hide the cursor during refresh
  abufAppend(&ab, "\x1b[?25l", 6);

  // this is writing 4 bytes to the terminal
  // the first is \x1b, the escape character
  // then the other three bytes are [2J
  // the stuff after the [ tells the terminal to do various things
  // J clears the screen, and the 2 means the entire screen
  // I'm using VT100 escape sequences here, they are supported most places
  // later it may change to ncurses
  // abufAppend(&ab, "\x1b[2J", 4); /* depreciated full screen refresh */
  // put the cursor at the top left
  // H is cursor position
  abufAppend(&ab, "\x1b[H", 3);

  DrawRows(&ab);

  // set the cursor position correctly
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abufAppend(&ab, buf, strlen(buf));

  // return the cursor after refresh
  abufAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abufFree(&ab);
  return;
}

void DrawRows(struct abuf *ab) {
  for (int i = 0; i < E.term_rows; ++i) {
    if (i >= E.numrows) {
      // display a welcome screen
      if (i == E.term_rows / 3) {
        // write the message
        char wel[30];
        int weln = snprintf(wel, sizeof(wel), "Abigail Editor uwu v%s", ABBY_VER);
        if (weln > E.term_cols) weln = E.term_cols;
        // center the message
        int pad = (E.term_cols - weln) / 2;
        if (pad != 0) {
          abufAppend(ab, "~", 1);
          --pad;
        }
        while (pad > 0) {
          abufAppend(ab, " ", 1);
          --pad;
        }

        abufAppend(ab, wel, weln);
      }
      else abufAppend(ab, "~", 1);
    }
    // actually draw the rows here
    else {
      int len = E.row.size;
      if (len > E.term_rows) len = E.term_cols;
      abufAppend(ab, E.row.chars, len);
    }





    // clear each line as it is drawn
    abufAppend(ab, "\x1b[K", 1);
    // stops terminal scroll on last line
    if (i < E.term_rows - 1) abufAppend(ab, "\r\n", 1);
  }
  return;
}

int WindowSize(int *rows, int *cols) {
  struct winsize ws;
  // ioctl will put the terminal rows and cols into ws, or return -1
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // but first a backup if ioctl doesn't work
    // the C command is cursor forward
    // the B command is cursor down
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    return CursorPosition(rows, cols);
  }
  else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

int CursorPosition(int *rows, int *cols) {
  // the n command is Device Status Report, 6 asks for cursor position
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  // setting up a timeout buffer for some systems
  // it also breaks on the 'R' character
  char buf[32];
  unsigned int i;
  for (i = 0; i < sizeof(buf) - 1; ++i) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
  }
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

void MoveCursor(char key) {
  switch (key) {
    case 65: // arrowkey up
      if (E.cy != 0) --E.cy;
      break;
    case 66: // arrowkey down
      if (E.cy != E.term_rows - 1) ++E.cy;
      break;
    case 67: // arrowkey right
      if (E.cx != E.term_cols - 1) ++E.cx;
      break;
    case 68: // arrowkey left
      if (E.cx != 0) --E.cx;
      break;
  }
  return;
}

void InitEditor() {
  // cursor coordinates
  E.cx = 0;
  E.cy = 0;

  E.numrows = 0;

  if (WindowSize(&E.term_rows, &E.term_cols) == -1) {
    Yamete("WindowSize");
  }
}

void abufAppend(struct abuf *ab, const char *s, int len) {
  char *neu = realloc(ab->b, ab->len + len);
  if (!neu) return;
  memcpy(&neu[ab->len], s, len);
  ab->b = neu;
  ab->len += len;
  return;
}

void abufFree(struct abuf *ab) {
  free(ab->b);
  return;
}

void OnEditOpen() {
  char *line = "It is working!";
  ssize_t linelen = 13;

  E.row.size = linelen;
  E.row.chars = malloc(linelen + 1);
  memcpy(E.row.chars, line, linelen);
  E.row.chars[linelen] = '\0';
  E.numrows = 1;
  return;
}





int main() {
  // setup abby
  StartRawMode();
  InitEditor();
  OnEditOpen();

  // read every input as it comes in
  while (1) {
    ScreenRefresh();
    ProcessKey();
  }
  return 0;
}
