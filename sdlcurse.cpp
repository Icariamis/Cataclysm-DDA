#if (defined TILES)
#include "catacurse.h"
#include "options.h"
#include "output.h"
#include "color.h"
#include "debug.h"
#include <cstdlib>
#include <fstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _MSC_VER
#include "wdirent.h"
#include <direct.h>
#else
#include <dirent.h>
#endif

// SDL headers end up in different places depending on the OS, sadly
#if (defined _WIN32 || defined WINDOWS)
#include "SDL.h"
#include "SDL_ttf.h"
#else
#include "SDL/SDL.h"
#include "SDL/SDL_ttf.h"
#include <wordexp.h>
#endif

//***********************************
//Globals                           *
//***********************************

WINDOW *mainwin;
static SDL_Color windowsPalette[256];
static SDL_Surface *screen = NULL;
static SDL_Surface *glyph_cache[128][16]; //cache ascii characters
TTF_Font* font;
static int ttf_height_hack = 0;
int nativeWidth;
int nativeHeight;
int WindowX;            //X pos of the actual window, not the curses window
int WindowY;            //Y pos of the actual window, not the curses window
int WindowWidth;        //Width of the actual window, not the curses window
int WindowHeight;       //Height of the actual window, not the curses window
int lastchar;          //the last character that was pressed, resets in getch
int inputdelay;         //How long getch will wait for a character to be typed
//WINDOW *_windows;  //Probably need to change this to dynamic at some point
//int WindowCount;        //The number of curses windows currently in use
int fontwidth;          //the width of the font, background is always this size
int fontheight;         //the height of the font, background is always this size
int halfwidth;          //half of the font width, used for centering lines
int halfheight;          //half of the font height, used for centering lines
pairs *colorpairs;   //storage for pair'ed colored, should be dynamic, meh
int echoOn;     //1 = getnstr shows input, 0 = doesn't show. needed for echo()-ncurses compatibility.

static unsigned long lastupdate = 0;
static unsigned long interval = 25;
static bool needupdate = false;

static bool fontblending = false;

//***********************************
//Non-curses, Window functions      *
//***********************************

void ClearScreen()
{
	SDL_FillRect(screen, NULL, 0);
}


bool fexists(const char *filename)
{
  std::ifstream ifile(filename);
  return ifile;
}

//Registers, creates, and shows the Window!!
bool WinCreate()
{
    if(OPTIONS[OPT_HIDE_CURSOR] > 0 && SDL_ShowCursor(-1))
        SDL_ShowCursor(SDL_DISABLE);
    else
        SDL_ShowCursor(SDL_ENABLE);
	const SDL_VideoInfo* video_info;
	int init_flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;

	if(SDL_Init(init_flags) < 0)
	{
		return false;
	}

	if(TTF_Init()<0)
	{
		return false;
	}

	SDL_EnableUNICODE(1);
	SDL_EnableKeyRepeat(500, 25);

	atexit(SDL_Quit);

	SDL_WM_SetCaption("Cataclysm: Dark Days Ahead - 0.6git", NULL);

	video_info = SDL_GetVideoInfo();
	nativeWidth = video_info->current_w;
	nativeHeight = video_info->current_h;

    char center_string[] = "SDL_VIDEO_CENTERED=center"; // indirection needed to avoid a warning
    SDL_putenv(center_string);
	screen = SDL_SetVideoMode(WindowWidth, WindowHeight, 32, (SDL_SWSURFACE|SDL_DOUBLEBUF));
	//SDL_SetColors(screen,windowsPalette,0,256);

	if(screen==NULL) return false;

	ClearScreen();

    return true;
};

void WinDestroy()
{
	if(screen) SDL_FreeSurface(screen);
	screen = NULL;
};

//The following 3 methods use mem functions for fast drawing
inline void VertLineDIB(int x, int y, int y2, int thickness, unsigned char color)
{
	SDL_Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = thickness;
	rect.h = y2-y;
    SDL_FillRect(screen, &rect, SDL_MapRGB(screen->format, windowsPalette[color].r,windowsPalette[color].g,windowsPalette[color].b));
};
inline void HorzLineDIB(int x, int y, int x2, int thickness, unsigned char color)
{
	SDL_Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = x2-x;
	rect.h = thickness;
    SDL_FillRect(screen, &rect, SDL_MapRGB(screen->format, windowsPalette[color].r,windowsPalette[color].g,windowsPalette[color].b));
};
inline void FillRectDIB(int x, int y, int width, int height, unsigned char color)
{
	SDL_Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = width;
	rect.h = height;
    SDL_FillRect(screen, &rect, SDL_MapRGB(screen->format, windowsPalette[color].r,windowsPalette[color].g,windowsPalette[color].b));
};


static void cache_glyphs()
{
    int top=999, bottom=-999;

    start_color();

    for(int ch=0; ch<128; ch++)
    {
        for(int color=0; color<16; color++)
        {
            SDL_Surface * glyph = glyph_cache[ch][color] = (fontblending?TTF_RenderGlyph_Blended:TTF_RenderGlyph_Solid)(font, ch, windowsPalette[color]);
            int minx, maxx, miny, maxy, advance;
            if(glyph!=NULL && color==0 && 0==TTF_GlyphMetrics(font, ch, &minx, &maxx, &miny, &maxy, &advance) )
            {
               int t = TTF_FontAscent(font)-maxy;
               int b = t + glyph->h;
               if(t<top) top = t;
               if(b>bottom) bottom = b;
            }
        }
    }

    int height = bottom - top;
    int delta = (fontheight-height)/2;

    ttf_height_hack =  delta - top;
}

static void OutputChar(char t, int x, int y, int n, unsigned char color)
{
    unsigned char ch = t & 0x7f;
    color &= 0xf;

    SDL_Surface * glyph = glyph_cache[ch][color];

    if(glyph)
    {
		int minx=0, maxy=0, dx=0, dy = 0;
		if( 0==TTF_GlyphMetrics(font, ch, &minx, NULL, NULL, &maxy, NULL))
		{
			dx = minx;
			dy = TTF_FontAscent(font)-maxy+ttf_height_hack;
			SDL_Rect rect;
			rect.x = x+dx; rect.y = y+dy; rect.w = fontwidth; rect.h = fontheight;
			SDL_BlitSurface(glyph, NULL, screen, &rect);
		}
    }
}

void try_update()
{
	unsigned long now=SDL_GetTicks();
	if(now-lastupdate>=interval)
	{
		SDL_UpdateRect(screen, 0, 0, screen->w, screen->h);
		needupdate = false;
		lastupdate = now;
	}
	else
	{
		needupdate = true;
	}
}

void DrawWindow(WINDOW *win)
{
    int i,j,drawx,drawy;
    char tmp;

    for (j=0; j<win->height; j++){
        if (win->line[j].touched)
        {
            needupdate = true;

            win->line[j].touched=false;

            for (i=0; i<win->width; i++){
                drawx=((win->x+i)*fontwidth);
                drawy=((win->y+j)*fontheight);//-j;
                if (((drawx+fontwidth)<=WindowWidth) && ((drawy+fontheight)<=WindowHeight)){
                tmp = win->line[j].chars[i];
                int FG = win->line[j].FG[i];
                int BG = win->line[j].BG[i];
                FillRectDIB(drawx,drawy,fontwidth,fontheight,BG);

                if ( tmp > 0){
                    OutputChar(tmp, drawx,drawy,1,FG);
                //    }     //and this line too.
                } else if (  tmp < 0 ) {
                    switch (tmp) {
                    case -60://box bottom/top side (horizontal line)
                        HorzLineDIB(drawx,drawy+halfheight,drawx+fontwidth,1,FG);
                        break;
                    case -77://box left/right side (vertical line)
                        VertLineDIB(drawx+halfwidth,drawy,drawy+fontheight,2,FG);
                        break;
                    case -38://box top left
                        HorzLineDIB(drawx+halfwidth,drawy+halfheight,drawx+fontwidth,1,FG);
                        VertLineDIB(drawx+halfwidth,drawy+halfheight,drawy+fontheight,2,FG);
                        break;
                    case -65://box top right
                        HorzLineDIB(drawx,drawy+halfheight,drawx+halfwidth,1,FG);
                        VertLineDIB(drawx+halfwidth,drawy+halfheight,drawy+fontheight,2,FG);
                        break;
                    case -39://box bottom right
                        HorzLineDIB(drawx,drawy+halfheight,drawx+halfwidth,1,FG);
                        VertLineDIB(drawx+halfwidth,drawy,drawy+halfheight+1,2,FG);
                        break;
                    case -64://box bottom left
                        HorzLineDIB(drawx+halfwidth,drawy+halfheight,drawx+fontwidth,1,FG);
                        VertLineDIB(drawx+halfwidth,drawy,drawy+halfheight+1,2,FG);
                        break;
                    case -63://box bottom north T (left, right, up)
                        HorzLineDIB(drawx,drawy+halfheight,drawx+fontwidth,1,FG);
                        VertLineDIB(drawx+halfwidth,drawy,drawy+halfheight,2,FG);
                        break;
                    case -61://box bottom east T (up, right, down)
                        VertLineDIB(drawx+halfwidth,drawy,drawy+fontheight,2,FG);
                        HorzLineDIB(drawx+halfwidth,drawy+halfheight,drawx+fontwidth,1,FG);
                        break;
                    case -62://box bottom south T (left, right, down)
                        HorzLineDIB(drawx,drawy+halfheight,drawx+fontwidth,1,FG);
                        VertLineDIB(drawx+halfwidth,drawy+halfheight,drawy+fontheight,2,FG);
                        break;
                    case -59://box X (left down up right)
                        HorzLineDIB(drawx,drawy+halfheight,drawx+fontwidth,1,FG);
                        VertLineDIB(drawx+halfwidth,drawy,drawy+fontheight,2,FG);
                        break;
                    case -76://box bottom east T (left, down, up)
                        VertLineDIB(drawx+halfwidth,drawy,drawy+fontheight,2,FG);
                        HorzLineDIB(drawx,drawy+halfheight,drawx+halfwidth,1,FG);
                        break;
                    default:
                        // SetTextColor(DC,_windows[w].line[j].chars[i].color.FG);
                        // TextOut(DC,drawx,drawy,&tmp,1);
                        break;
                    }
                    };//switch (tmp)
                }//(tmp < 0)
            };//for (i=0;i<_windows[w].width;i++)
        }
    };// for (j=0;j<_windows[w].height;j++)
    win->draw=false;                //We drew the window, mark it as so

    if (needupdate) try_update();
}

//Check for any window messages (keypress, paint, mousemove, etc)
void CheckMessages()
{
	SDL_Event ev;
    bool quit = false;
	while(SDL_PollEvent(&ev))
	{
		switch(ev.type)
		{
			case SDL_KEYDOWN:
			{
			    if(OPTIONS[OPT_HIDE_CURSOR] > 0 && SDL_ShowCursor(-1)) SDL_ShowCursor(SDL_DISABLE); //hide mouse cursor on keyboard input
				Uint8 *keystate = SDL_GetKeyState(NULL);
				// manually handle Alt+F4 for older SDL lib, no big deal
				if(ev.key.keysym.sym==SDLK_F4 && (keystate[SDLK_RALT] || keystate[SDLK_LALT]) )
				{
					quit = true;
					break;
				}
				else if(ev.key.keysym.sym==SDLK_RSHIFT || ev.key.keysym.sym==SDLK_LSHIFT ||
					ev.key.keysym.sym==SDLK_RCTRL || ev.key.keysym.sym==SDLK_LCTRL ||
					ev.key.keysym.sym==SDLK_RALT || ev.key.keysym.sym==SDLK_LALT)
				{
					break; // temporary fix for unwanted keys
				}
				else if (ev.key.keysym.unicode != 0) {
					lastchar = ev.key.keysym.unicode;
					switch (lastchar){
						case 13:            //Reroute ENTER key for compatilbity purposes
							lastchar=10;
							break;
						case 8:             //Reroute BACKSPACE key for compatilbity purposes
							lastchar=127;
							break;
					}
				}
				if(ev.key.keysym.sym==SDLK_LEFT) {
					lastchar = KEY_LEFT;
				}
				else if(ev.key.keysym.sym==SDLK_RIGHT) {
					lastchar = KEY_RIGHT;
				}
				else if(ev.key.keysym.sym==SDLK_UP) {
					lastchar = KEY_UP;
				}
				else if(ev.key.keysym.sym==SDLK_DOWN) {
					lastchar = KEY_DOWN;
				}
				else if(ev.key.keysym.sym==SDLK_PAGEUP) {
					lastchar = KEY_PPAGE;
				}
				else if(ev.key.keysym.sym==SDLK_PAGEDOWN) {
					lastchar = KEY_NPAGE;

				}
			}
			break;
			case SDL_MOUSEMOTION:
                if((OPTIONS[OPT_HIDE_CURSOR] == 0 || OPTIONS[OPT_HIDE_CURSOR] == 2) &&
                    !SDL_ShowCursor(-1)) SDL_ShowCursor(SDL_ENABLE);
                break;
			case SDL_QUIT:
                quit = true;
				break;

		}
	}
    if (needupdate) try_update();
    if(quit)
    {
        endwin();
        exit(0);
    }
}

//***********************************
//Psuedo-Curses Functions           *
//***********************************

static void font_folder_list(std::ofstream& fout, std::string path)
{
	DIR *dir;
	struct dirent *ent;
	if ((dir = opendir (path.c_str())) != NULL) {
		bool found = false;
		while (!found && (ent = readdir (dir)) != NULL) {
   if( 0 == strcmp( ent->d_name, "." ) ||
       0 == strcmp( ent->d_name, ".." ) ) {
       continue;
   }
			std::string f = path + "/" + ent->d_name;
   struct stat stat_buffer;
   if( stat( f.c_str(), &stat_buffer ) == -1 ) {
       continue;
   }
   if( S_ISDIR(stat_buffer.st_mode) ) {
    font_folder_list( fout, f );
    continue;
   }
			TTF_Font* fnt = TTF_OpenFont(f.c_str(), 12);
			long nfaces = 0;
			if(fnt)
			{
				nfaces = TTF_FontFaces(fnt);
				TTF_CloseFont(fnt);
			}
			for(long i=0; i<nfaces; i++)
			{
				fnt = TTF_OpenFontIndex(f.c_str(), 12, i);
				if(fnt)
				{
					char *fami = TTF_FontFaceFamilyName(fnt);
					char *style = TTF_FontFaceStyleName(fnt);
					if(fami)
					{
						fout << fami << std::endl;
						fout << f << std::endl;
						fout << i << std::endl;
					}
					if(fami && style)
					{
						fout << fami << " " << style << std::endl;
						fout << f << std::endl;
						fout << i << std::endl;
					}
					TTF_CloseFont(fnt);
				}
			}
		}
		closedir (dir);
	}

}

#if (defined _WIN32 || defined WINDOWS)
#include <windows.h>
#elif (defined _APPLE_ && defined _MACH_)
//#include <wordexp.h> //not tested
#elif (defined linux || defined __linux)
//#include <wordexp.h>
#endif

static void save_font_list()
{
	std::ofstream fout("data/fontlist.txt", std::ios_base::trunc);

	font_folder_list(fout, "data/font");

#if (defined _WIN32 || defined WINDOWS)
	char buf[256];
	//DebugLog log;
	GetSystemWindowsDirectory(buf, 256);
	strcat(buf, "\\fonts");
	font_folder_list(fout, buf);
#elif (defined _APPLE_ && defined _MACH_)

	/*
	// Well I don't know how osx actually works ....
	font_folder_list(fout, "/System/Library/Fonts");
	font_folder_list(fout, "/Library/Fonts");

	wordexp_t exp;
	wordexp("~/Library/Fonts", &exp, 0);
	font_folder_list(fout, exp.we_wordv[0]);
	wordfree(&exp);*/
#elif (defined linux || defined __linux)
	font_folder_list(fout, "/usr/share/fonts");
	font_folder_list(fout, "/usr/local/share/fonts");
	wordexp_t exp;
	wordexp("~/.fonts", &exp, 0);
	font_folder_list(fout, exp.we_wordv[0]);
	wordfree(&exp);
#endif
	//TODO: other systems

	fout << "end of list" << std::endl;

}

static std::string find_system_font(std::string name, int& faceIndex)
{
	if(!fexists("data/fontlist.txt")) save_font_list();

	std::ifstream fin("data/fontlist.txt");
	if(fin)
	{
		std::string fname;
		std::string fpath;
		std::string iline;
		int index = 0;
		do
		{
			getline(fin, fname);
			if(fname=="end of list") break;
			getline(fin, fpath);
			getline(fin, iline);
			index = atoi(iline.c_str());
   if(0==strcasecmp(fname.c_str(), name.c_str()))
			{
				faceIndex = index;
				return fpath;
			}
		}while(true);
	}

	return "";
}


//Basic Init, create the font, backbuffer, etc
WINDOW *initscr(void)
{
    lastchar=-1;
    inputdelay=-1;

    std::string typeface = "";
	std::string blending = "solid";
	std::ifstream fin;
	int faceIndex = 0; 
    int fontsize = 0; //actuall size
	fin.open("data/FONTDATA");
	if (!fin.is_open()){
        fontheight=16;
        fontwidth=8;
	} else {
        getline(fin, typeface);
        fin >> fontwidth;
        fin >> fontheight;
        fin >> fontsize;
		fin >> blending;
        if ((fontwidth <= 4) || (fontheight <=4)){
            fontheight=16;
            fontwidth=8;
		}
		fin.close();
	}

	fontblending = (blending=="blended");

    halfwidth=fontwidth / 2;
    halfheight=fontheight / 2;
    WindowWidth= (55 + (OPTIONS[OPT_VIEWPORT_X] * 2 + 1)) * fontwidth;
    WindowHeight= (OPTIONS[OPT_VIEWPORT_Y] * 2 + 1) *fontheight;
    if(!WinCreate()) {}// do something here

	std::string sysfnt = find_system_font(typeface, faceIndex);
	if(sysfnt!="") typeface = sysfnt;

    //make fontdata compatible with wincurse
    if(!fexists(typeface.c_str())) {
		faceIndex = 0;
        typeface = "data/font/" + typeface + ".ttf";
	}

    //different default font with wincurse
    if(!fexists(typeface.c_str())) {
		faceIndex = 0;
        typeface = "data/font/fixedsys.ttf";
	}

    if(fontsize<=0) fontsize=fontheight-1;
	font = TTF_OpenFontIndex(typeface.c_str(), fontsize, faceIndex);

    //if(!font) something went wrong

	TTF_SetFontStyle(font, TTF_STYLE_NORMAL);

	// glyph height hack by utunnels
	// SDL_ttf doesn't use FT_HAS_VERTICAL for function TTF_GlyphMetrics
	// this causes baseline problems for certain fonts
	// I can only guess by check a certain tall character...
    cache_glyphs();

    mainwin = newwin((OPTIONS[OPT_VIEWPORT_Y] * 2 + 1),(55 + (OPTIONS[OPT_VIEWPORT_Y] * 2 + 1)),0,0);
    return mainwin;   //create the 'stdscr' window and return its ref
}

WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x)
{
    if (begin_y < 0 || begin_x < 0) {
        return NULL; //it's the caller's problem now (since they have logging functions declared)
    }

    // default values
    if (ncols == 0)
    {
        ncols = TERMX - begin_x;
    }
    if (nlines == 0)
    {
        nlines = TERMY - begin_y;
    }

    int i,j;
    WINDOW *newwindow = new WINDOW;
    //newwindow=&_windows[WindowCount];
    newwindow->x=begin_x;
    newwindow->y=begin_y;
    newwindow->width=ncols;
    newwindow->height=nlines;
    newwindow->inuse=true;
    newwindow->draw=false;
    newwindow->BG=0;
    newwindow->FG=8;
    newwindow->cursorx=0;
    newwindow->cursory=0;
    newwindow->line = new curseline[nlines];

    for (j=0; j<nlines; j++)
    {
        newwindow->line[j].chars= new char[ncols];
        newwindow->line[j].FG= new char[ncols];
        newwindow->line[j].BG= new char[ncols];
        newwindow->line[j].touched=true;//Touch them all !?
        for (i=0; i<ncols; i++)
        {
          newwindow->line[j].chars[i]=0;
          newwindow->line[j].FG[i]=0;
          newwindow->line[j].BG[i]=0;
        }
    }
    //WindowCount++;
    return newwindow;
}

//Deletes the window and marks it as free. Clears it just in case.
int delwin(WINDOW *win)
{
    int j;
    win->inuse=false;
    win->draw=false;
    for (j=0; j<win->height; j++){
        delete win->line[j].chars;
        delete win->line[j].FG;
        delete win->line[j].BG;
        }
    delete win->line;
    delete win;
    return 1;
}

inline int newline(WINDOW *win){
    if (win->cursory < win->height - 1){
        win->cursory++;
        win->cursorx=0;
        return 1;
    }
return 0;
}

inline void addedchar(WINDOW *win){
    win->cursorx++;
    win->line[win->cursory].touched=true;
    if (win->cursorx > win->width)
        newline(win);
}


//Borders the window with fancy lines!
int wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs, chtype tl, chtype tr, chtype bl, chtype br)
{
/*
ncurses does not do this, and this prevents: wattron(win, c_customBordercolor); wborder(win, ...); wattroff(win, c_customBorderColor);
    wattron(win, c_white);
*/
    int i, j;
    int oldx=win->cursorx;//methods below move the cursor, save the value!
    int oldy=win->cursory;//methods below move the cursor, save the value!
    if (ls>0)
        for (j=1; j<win->height-1; j++)
            mvwaddch(win, j, 0, 179);
    if (rs>0)
        for (j=1; j<win->height-1; j++)
            mvwaddch(win, j, win->width-1, 179);
    if (ts>0)
        for (i=1; i<win->width-1; i++)
            mvwaddch(win, 0, i, 196);
    if (bs>0)
        for (i=1; i<win->width-1; i++)
            mvwaddch(win, win->height-1, i, 196);
    if (tl>0)
        mvwaddch(win,0, 0, 218);
    if (tr>0)
        mvwaddch(win,0, win->width-1, 191);
    if (bl>0)
        mvwaddch(win,win->height-1, 0, 192);
    if (br>0)
        mvwaddch(win,win->height-1, win->width-1, 217);
    //_windows[w].cursorx=oldx;//methods above move the cursor, put it back
    //_windows[w].cursory=oldy;//methods above move the cursor, put it back
    wmove(win,oldy,oldx);
    wattroff(win, c_white);
    return 1;
}

//Refreshes a window, causing it to redraw on top.
int wrefresh(WINDOW *win)
{
    if (win==0) win=mainwin;
    if (win->draw)
        DrawWindow(win);
    return 1;
}

//Refreshes window 0 (stdscr), causing it to redraw on top.
int refresh(void)
{
    return wrefresh(mainwin);
}

int getch(void)
{
    return wgetch(mainwin);
}

//Not terribly sure how this function is suppose to work,
//but jday helped to figure most of it out
int wgetch(WINDOW* win)
{
	// standards note: getch is sometimes required to call refresh
	// see, e.g., http://linux.die.net/man/3/getch
	// so although it's non-obvious, that refresh() call (and maybe InvalidateRect?) IS supposed to be there
	wrefresh(win);
	lastchar=ERR;//ERR=-1
    if (inputdelay < 0)
	{
        do
        {
            CheckMessages();
            if (lastchar!=ERR) break;
            SDL_Delay(1);
        }
        while (lastchar==ERR);
	}
    else if (inputdelay > 0)
	{
        unsigned long starttime=SDL_GetTicks();
        unsigned long endtime;
        do
        {
            CheckMessages();
            endtime=SDL_GetTicks();
            if (lastchar!=ERR) break;
            SDL_Delay(1);
        }
        while (endtime<(starttime+inputdelay));
	}
	else
	{
		CheckMessages();
	}
    return lastchar;
}

int mvgetch(int y, int x)
{
    move(y,x);
    return getch();
}

int mvwgetch(WINDOW* win, int y, int x)
{
    move(y, x);
    return wgetch(win);
}

int getnstr(char *str, int size)
{
    int startX = mainwin->cursorx;
    int count = 0;
    char input;
    while(true)
    {
	input = getch();
	// Carriage return, Line feed and End of File terminate the input.
	if( input == '\r' || input == '\n' || input == '\x04' )
	{
	    str[count] = '\x00';
	    return count;
	}
	else if( input == 127 ) // Backspace, remapped from \x8 in ProcessMessages()
	{
	    if( count == 0 )
		continue;
	    str[count] = '\x00';
	    if(echoOn == 1)
	        mvaddch(mainwin->cursory, startX + count, ' ');
	    --count;
	    if(echoOn == 1)
	      move(mainwin->cursory, startX + count);
	}
	else
	{
	    if( count >= size - 1 ) // Still need space for trailing 0x00
	        continue;
	    str[count] = input;
	    ++count;
	    if(echoOn == 1)
	    {
	        move(mainwin->cursory, startX + count);
            mvaddch(mainwin->cursory, startX + count, input);
	    }
	}
    }
    return count;
}
//The core printing function, prints characters to the array, and sets colors
inline int printstring(WINDOW *win, char *fmt)
{
 int size = strlen(fmt);
 int j;
 for (j=0; j<size; j++){
  if (!(fmt[j]==10)){//check that this isnt a newline char
   if (win->cursorx <= win->width - 1 && win->cursory <= win->height - 1) {
    win->line[win->cursory].chars[win->cursorx]=fmt[j];
    win->line[win->cursory].FG[win->cursorx]=win->FG;
    win->line[win->cursory].BG[win->cursorx]=win->BG;
    win->line[win->cursory].touched=true;
    addedchar(win);
   } else if (win->cursory <= win->height - 1) {
     // don't write outside the window, but don't abort if there are still lines to write.
   } else {
     return 0; //if we try and write anything outside the window, abort completely
   }
  } else // if the character is a newline, make sure to move down a line
  if (newline(win)==0){
   return 0;
  }
 }
 win->draw=true;
 return 1;
}

//Prints a formatted string to a window at the current cursor, base function
int wprintw(WINDOW *win, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char printbuf[2048];
    vsnprintf(printbuf, 2047, fmt, args);
    va_end(args);
    return printstring(win,printbuf);
};

//Prints a formatted string to a window, moves the cursor
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char printbuf[2048];
    vsnprintf(printbuf, 2047, fmt, args);
    va_end(args);
    if (wmove(win,y,x)==0) return 0;
    return printstring(win,printbuf);
};

//Prints a formatted string to window 0 (stdscr), moves the cursor
int mvprintw(int y, int x, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char printbuf[2048];
    vsnprintf(printbuf, 2047, fmt, args);
    va_end(args);
    if (move(y,x)==0) return 0;
    return printstring(mainwin,printbuf);
};

//Prints a formatted string to window 0 (stdscr) at the current cursor
int printw(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char printbuf[2078];
    vsnprintf(printbuf, 2047, fmt, args);
    va_end(args);
    return printstring(mainwin,printbuf);
};

//erases a window of all text and attributes
int werase(WINDOW *win)
{
    int j,i;
    for (j=0; j<win->height; j++)
    {
     for (i=0; i<win->width; i++)   {
     win->line[j].chars[i]=0;
     win->line[j].FG[i]=0;
     win->line[j].BG[i]=0;
     }
        win->line[j].touched=true;
    }
    win->draw=true;
    wmove(win,0,0);
//    wrefresh(win);
    return 1;
};

//erases window 0 (stdscr) of all text and attributes
int erase(void)
{
    return werase(mainwin);
};

//pairs up a foreground and background color and puts it into the array of pairs
int init_pair(short pair, short f, short b)
{
    colorpairs[pair].FG=f;
    colorpairs[pair].BG=b;
    return 1;
};

//moves the cursor in a window
int wmove(WINDOW *win, int y, int x)
{
    if (x>=win->width)
     {return 0;}//FIXES MAP CRASH -> >= vs > only
    if (y>=win->height)
     {return 0;}// > crashes?
    if (y<0)
     {return 0;}
    if (x<0)
     {return 0;}
    win->cursorx=x;
    win->cursory=y;
    return 1;
};

//Clears windows 0 (stdscr)     I'm not sure if its suppose to do this?
int clear(void)
{
    return wclear(mainwin);
};

//Ends the terminal, destroy everything
int endwin(void)
{
	TTF_CloseFont(font);
	for(int i=0; i<128; i++)
	{
		for(int j=0; j<16; j++)
		{
			if(glyph_cache[i][j]) SDL_FreeSurface(glyph_cache[i][j]);
			glyph_cache[i][j] = NULL;
		}
	}
    WinDestroy();
    return 1;
};

//adds a character to the window
int mvwaddch(WINDOW *win, int y, int x, const chtype ch)
{
   if (wmove(win,y,x)==0) return 0;
   return waddch(win, ch);
};

//clears a window
int wclear(WINDOW *win)
{
    werase(win);
    clearok(win);
    return 1;
};

int clearok(WINDOW *win)
{
    for (int i=0; i<win->y; i++)
    {
        win->line[i].touched = true;
    }
    return 1;
}

//gets the max x of a window (the width)
int getmaxx(WINDOW *win)
{
    if (win==0) return mainwin->width;     //StdScr
    return win->width;
};

//gets the max y of a window (the height)
int getmaxy(WINDOW *win)
{
    if (win==0) return mainwin->height;     //StdScr
    return win->height;
};

//gets the beginning x of a window (the x pos)
int getbegx(WINDOW *win)
{
    if (win==0) return mainwin->x;     //StdScr
    return win->x;
};

//gets the beginning y of a window (the y pos)
int getbegy(WINDOW *win)
{
    if (win==0) return mainwin->y;     //StdScr
    return win->y;
};

//copied from gdi version and don't bother to rename it
inline SDL_Color BGR(int b, int g, int r)
{
    SDL_Color result;
    result.b=b;    //Blue
    result.g=g;    //Green
    result.r=r;    //Red
    //result.a=0;//The Alpha, isnt used, so just set it to 0
    return result;
};

int start_color(void)
{
	colorpairs=new pairs[50];
	windowsPalette[0]= BGR(0,0,0); // Black
	windowsPalette[1]= BGR(0, 0, 255); // Red
	windowsPalette[2]= BGR(0,110,0); // Green
	windowsPalette[3]= BGR(23,51,92); // Brown???
	windowsPalette[4]= BGR(200, 0, 0); // Blue
	windowsPalette[5]= BGR(98, 58, 139); // Purple
	windowsPalette[6]= BGR(180, 150, 0); // Cyan
	windowsPalette[7]= BGR(150, 150, 150);// Gray
	windowsPalette[8]= BGR(99, 99, 99);// Dark Gray
	windowsPalette[9]= BGR(150, 150, 255); // Light Red/Salmon?
	windowsPalette[10]= BGR(0, 255, 0); // Bright Green
	windowsPalette[11]= BGR(0, 255, 255); // Yellow
	windowsPalette[12]= BGR(255, 100, 100); // Light Blue
	windowsPalette[13]= BGR(240, 0, 255); // Pink
	windowsPalette[14]= BGR(255, 240, 0); // Light Cyan?
	windowsPalette[15]= BGR(255, 255, 255); //White
	//SDL_SetColors(screen,windowsPalette,0,256);
	return 0;
};

int keypad(WINDOW *faux, bool bf)
{
return 1;
};

int cbreak(void)
{
    return 1;
};
int keypad(int faux, bool bf)
{
    return 1;
};
int curs_set(int visibility)
{
    return 1;
};

int mvaddch(int y, int x, const chtype ch)
{
    return mvwaddch(mainwin,y,x,ch);
};

int wattron(WINDOW *win, int attrs)
{
    bool isBold = !!(attrs & A_BOLD);
    bool isBlink = !!(attrs & A_BLINK);
    int pairNumber = (attrs & A_COLOR) >> 17;
    win->FG=colorpairs[pairNumber].FG;
    win->BG=colorpairs[pairNumber].BG;
    if (isBold) win->FG += 8;
    if (isBlink) win->BG += 8;
    return 1;
};
int wattroff(WINDOW *win, int attrs)
{
     win->FG=8;                                  //reset to white
     win->BG=0;                                  //reset to black
    return 1;
};
int attron(int attrs)
{
    return wattron(mainwin, attrs);
};
int attroff(int attrs)
{
    return wattroff(mainwin,attrs);
};
int waddch(WINDOW *win, const chtype ch)
{
    char charcode;
    charcode=ch;

    switch (ch){        //LINE_NESW  - X for on, O for off
        case 4194424:   //#define LINE_XOXO 4194424
            charcode=179;
            break;
        case 4194417:   //#define LINE_OXOX 4194417
            charcode=196;
            break;
        case 4194413:   //#define LINE_XXOO 4194413
            charcode=192;
            break;
        case 4194412:   //#define LINE_OXXO 4194412
            charcode=218;
            break;
        case 4194411:   //#define LINE_OOXX 4194411
            charcode=191;
            break;
        case 4194410:   //#define LINE_XOOX 4194410
            charcode=217;
            break;
        case 4194422:   //#define LINE_XXOX 4194422
            charcode=193;
            break;
        case 4194420:   //#define LINE_XXXO 4194420
            charcode=195;
            break;
        case 4194421:   //#define LINE_XOXX 4194421
            charcode=180;
            break;
        case 4194423:   //#define LINE_OXXX 4194423
            charcode=194;
            break;
        case 4194414:   //#define LINE_XXXX 4194414
            charcode=197;
            break;
        default:
            charcode = (char)ch;
            break;
        }


int curx=win->cursorx;
int cury=win->cursory;

//if (win2 > -1){
   win->line[cury].chars[curx]=charcode;
   win->line[cury].FG[curx]=win->FG;
   win->line[cury].BG[curx]=win->BG;


    win->draw=true;
    addedchar(win);
    return 1;
  //  else{
  //  win2=win2+1;

};




//Move the cursor of windows 0 (stdscr)
int move(int y, int x)
{
    return wmove(mainwin,y,x);
};

//Set the amount of time getch waits for input
void timeout(int delay)
{
    inputdelay=delay;
};
void set_escdelay(int delay) { } //PORTABILITY, DUMMY FUNCTION


int echo()
{
    echoOn = 1;
    return 0; // 0 = OK, -1 = ERR
}

int noecho()
{
    echoOn = 0;
    return 0;
}

#endif // TILES
