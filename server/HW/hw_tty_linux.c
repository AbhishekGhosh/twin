
/*
 * Linux console, linux terminal and twin terminal drivers:
 * 
 * plain Unix-style tty keyboard input;
 * 
 * mouse input uses libgpm to connect to `gpm' mouse daemon
 * and read mouse state, but draws mouse pointer manually;
 * (as fallback, xterm-style mouse input)
 * 
 * output through /dev/vcsaXX;
 * (as fallback, output through stdout)
 *
 * CONF_HW_TTY_LINUX is for linux console support (gpm, /dev/vcsa*), while
 * CONF_HW_TTY_TWTERM is for twin terminal (stdout, xterm style mouse)
 */
#ifdef CONF_HW_TTY_LINUX

/*
 * libgpm is stacked, not multi-headed (so no multiplex too)
 */
static byte GPM_InUse;


/*
 * Linux gpm mouse input:
 * mouse input uses libgpm to connect to `gpm' mouse daemon
 * and read mouse state, but draws mouse pointer manually
 */

static void GPM_QuitMouse(void);
static void GPM_MouseEvent(int fd, display_hw *hw);

static int wrap_GPM_Open(void) {
    /*
     * HACK! this works around a quirk in libgpm:
     * if Gpm_Open fails, it sets gpm_tried to non-zero
     * and following calls will just fail, not even trying anymore
     */
    extern int gpm_tried;
    
    if (!tty_name) {
	fputs("      GPM_InitMouse() failed: unable to detect tty device\n", stderr);
	return NOFD;
    }
    if (tty_number < 1 || tty_number > 63) {
	fprintf(stderr, "      GPM_InitMouse() failed: terminal `%s'\n"
		        "      is not a local linux console.\n", tty_name);
	return NOFD;
    }

    gpm_tried = 0;
    
    gpm_zerobased = 1;
    gpm_visiblepointer = 0;
    GPM_Conn.eventMask = ~0;		/* Get everything */
    GPM_Conn.defaultMask = ~GPM_HARD;	/* Pass everything unused */
    GPM_Conn.minMod = 0;		/* Run always... */
    GPM_Conn.maxMod = ~0;		/* ...with any modifier */

    GPM_fd = Gpm_Open(&GPM_Conn, tty_number);
    
    if (GPM_fd >= 0) {
	/* gpm_consolefd is opened by GPM_Open() */
	fcntl(gpm_consolefd, F_SETFD, FD_CLOEXEC);
    } else {
	fputs("      GPM_InitMouse() failed: unable to connect to `gpm'.\n"
	      "      make sure you started `twin' from the console\n"
	      "      and/or check that `gpm' is running.\n", stderr);
    }
    return GPM_fd;
}



/* return FALSE if failed */
static byte GPM_InitMouse(void) {
    
    if (GPM_InUse) {
	fputs("      GPM_InitMouse() failed: already connected to `gpm'.\n", stderr);
	return FALSE;
    }
    
    if (wrap_GPM_Open() < 0)
	return FALSE;

    fcntl(GPM_fd, F_SETFD, FD_CLOEXEC);
    fcntl(GPM_fd, F_SETFL, O_NONBLOCK);
    
    HW->mouse_slot = RegisterRemote(GPM_fd, HW, (void *)GPM_MouseEvent);
    if (HW->mouse_slot == NOSLOT) {
	Gpm_Close();
	return FALSE;
    }
    
    HW->FlagsHW |= FlHWSoftMouse; /* _we_ Hide/Show it */
    
    HW->MouseEvent = GPM_MouseEvent;
    HW->QuitMouse = GPM_QuitMouse;

    GPM_InUse = TRUE;
    
    return TRUE;
}

static void GPM_QuitMouse(void) {
    HW->HideMouse();
    Gpm_Close();

    UnRegisterRemote(HW->mouse_slot);
    HW->mouse_slot = NOSLOT;

    GPM_InUse = FALSE;
    
    HW->QuitMouse = NoOp;
}

static void GPM_MouseEvent(int fd, display_hw *hw) {
    udat IdButtons, Buttons = 0;
    Gpm_Event GPM_EV;
    
    SaveHW;
    
    /*
     * All other parts of twin read and parse data from fds in big chunks,
     * while Gpm_GetEvent() reads and parses only a single event at time.
     * To compensate this and avoid mouse to lag behind, we do a small loop.
     */
    byte loopN = 30;
    
    SetHW(hw);
    
    while (loopN--) {
	if (Gpm_GetEvent(&GPM_EV) <= 0) {
	    if (loopN == 29)
		HW->NeedHW |= NEEDPanicHW, NeedHW |= NEEDPanicHW;
	    return;
	}
	
#if 0
	All->FullShiftFlags =
	    (GPM_EV.modifiers & 1 ? FULL_LEFT_SHIFT_PRESSED : 0)
	    | (GPM_EV.modifiers & 2 ? FULL_RIGHT_ALT_PRESSED  : 0)
	    | (GPM_EV.modifiers & 4 ? FULL_LEFT_CTRL_PRESSED  : 0)
	    | (GPM_EV.modifiers & 8 ? FULL_LEFT_ALT_PRESSED   : 0);
#endif
	
	/*
	 * Gpm differs from us about what buttons to report on release events:
	 * it reports which buttons get _released_, not which are still _pressed_
	 * Fixed here. SIGH.
	 */
	IdButtons = GPM_EV.buttons;
	
	if (GPM_EV.type & GPM_UP)
	    IdButtons = GPM_keys & ~IdButtons;
	GPM_keys = IdButtons;
	
	if (IdButtons & GPM_B_LEFT)
	    Buttons |= HOLD_LEFT;
	if (IdButtons & GPM_B_MIDDLE)
	    Buttons |= HOLD_MIDDLE;
	if (IdButtons & GPM_B_RIGHT)
	    Buttons |= HOLD_RIGHT;
	
	MouseEventCommon(GPM_EV.x, GPM_EV.y, GPM_EV.dx, GPM_EV.dy, Buttons);
    }
    RestoreHW;
}


#endif /* CONF_HW_TTY_LINUX */





#if defined (CONF_HW_TTY_LINUX) || defined(CONF_HW_TTY_TWTERM)

static void linux_Beep(void);
static void linux_Configure(udat resource, byte todefault, udat value);
static void linux_SetPalette(udat N, udat R, udat G, udat B);
static void linux_ResetPalette(void);
static void linux_UpdateMouseAndCursor(void);

static byte linux_CanDragArea(dat Left, dat Up, dat Rgt, dat Dwn, dat DstLeft, dat DstUp);
static void linux_DragArea(dat Left, dat Up, dat Rgt, dat Dwn, dat DstLeft, dat DstUp);

INLINE void linux_SetCursorType(uldat type) {
    fprintf(stdOUT, "\033[?%d;%d;%dc",
		(int)(type & 0xFF),
		(int)((type >> 8) & 0xFF),
		(int)((type >> 16) & 0xFF));
}
INLINE void linux_MoveToXY(udat x, udat y) {
    fprintf(stdOUT, "\033[%d;%dH", y+1, x+1);
}

#ifdef CONF_HW_TTY_LINUX

/* output through /dev/vcsaXX */

static void vcsa_QuitVideo(void);
static void vcsa_FlushVideo(void);
static void vcsa_ShowMouse(void);
static void vcsa_HideMouse(void);

/* return FALSE if failed */
static byte vcsa_InitVideo(void) {
    static byte vcsa_name[] = "/dev/vcsaXX";
    
    if (!tty_name) {
	fputs("      vcsa_InitVideo() failed: unable to detect tty device\n", stderr);
	return FALSE;
    }
    
    if (tty_number < 1 || tty_number > 63) {
	fprintf(stderr, "      vcsa_InitVideo() failed: terminal `%s'\n"
			 "      is not a local linux console.\n", tty_name);
	return FALSE;
    }

    vcsa_name[9] = tty_name[8];
    vcsa_name[10] = tty_name[9];
    
    GetPrivileges();
    VcsaFd = open(vcsa_name, O_WRONLY|O_NOCTTY);
    DropPrivileges();
    
    if (VcsaFd < 0) {
	fprintf(stderr, "      vcsa_InitVideo() failed: unable to open `%s': %s\n",
		vcsa_name, strerror(errno));
	return FALSE;
    }
    fcntl(VcsaFd, F_SETFD, FD_CLOEXEC);
    
    fputs("\033[2J", stdOUT); /* clear screen */
    fflush(stdOUT);
    
    HW->FlushVideo = vcsa_FlushVideo;
    HW->FlushHW = stdout_FlushHW;

    HW->ShowMouse = vcsa_ShowMouse;
    HW->HideMouse = vcsa_HideMouse;
    HW->UpdateMouseAndCursor = linux_UpdateMouseAndCursor;

    HW->DetectSize  = stdin_DetectSize; stdin_DetectSize(&HW->usedX, &HW->usedY);
    HW->CheckResize = stdin_CheckResize;
    HW->Resize      = stdin_Resize;
    
    HW->HWSelectionImport  = AlwaysFalse;
    HW->HWSelectionExport  = NoOp;
    HW->HWSelectionRequest = (void *)NoOp;
    HW->HWSelectionNotify  = (void *)NoOp;
    HW->HWSelectionPrivate = NULL;
    
    HW->CanDragArea = linux_CanDragArea;
    HW->DragArea = linux_DragArea;
   
    HW->XY[0] = HW->XY[1] = 0;
    HW->TT = (uldat)-1; /* force updating cursor */
    
    HW->Beep = linux_Beep;
    HW->Configure = linux_Configure;
    HW->SetPalette = linux_SetPalette;
    HW->ResetPalette = linux_ResetPalette;

    HW->QuitVideo = vcsa_QuitVideo;
    
    HW->FlagsHW |= FlHWNeedOldVideo;
    HW->FlagsHW &= ~FlHWExpensiveFlushVideo;
    HW->NeedHW = 0;
    HW->CanResize = FALSE;
    HW->merge_Threshold = 40;

    return TRUE;
}

static void vcsa_QuitVideo(void) {
    linux_MoveToXY(0, ScreenHeight-1);
    linux_SetCursorType(LINECURSOR);
    fputs("\033[0m\033[3l\n", stdOUT); /* clear colors, TTY_DISPCTRL */
    
    close(VcsaFd);
    
    HW->QuitVideo = NoOp;
}


static void vcsa_FlushVideo(void) {
    dat i, j;
    uldat prevS = (uldat)-1, prevE = (uldat)-1, _prevS, _prevE, _start, _end, start, end;
    byte FlippedVideo = FALSE, FlippedOldVideo = FALSE;
    hwattr savedOldVideo;
    
    if (!ChangedVideoFlag) {
	HW->UpdateMouseAndCursor();
	return;
    }
    
    /* this hides the mouse if needed ... */
    
    /* first, check the old mouse position */
    if (HW->FlagsHW & FlHWSoftMouse) {
	if (HW->FlagsHW & FlHWChangedMouseFlag) {
	    /* dirty the old mouse position, so that it will be overwritten */
	    
	    /*
	     * with multi-display this is a hack, but since OldVideo gets restored
	     * by VideoFlip() below *BEFORE* returning from vcsa_FlushVideo(),
	     * that's ok.
	     */
	    DirtyVideo(HW->Last_x, HW->Last_y, HW->Last_x, HW->Last_y);
	    if (ValidOldVideo) {
		FlippedOldVideo = TRUE;
		savedOldVideo = OldVideo[HW->Last_x + HW->Last_y * ScreenWidth];
		OldVideo[HW->Last_x + HW->Last_y * ScreenWidth] = ~Video[HW->Last_x + HW->Last_y * ScreenWidth];
	    }
	}
	
	i = HW->MouseState.x;
	j = HW->MouseState.y;
	/*
	 * instead of calling ShowMouse(),
	 * we flip the new mouse position in Video[] and dirty it if necessary.
	 * this avoids glitches if the mouse is between two dirty areas
	 * that get merged.
	 */
	if ((HW->FlagsHW & FlHWChangedMouseFlag) || (FlippedVideo = Threshold_isDirtyVideo(i, j))) {
	    VideoFlip(i, j);
	    if (!FlippedVideo)
		DirtyVideo(i, j, i, j);
	    HW->FlagsHW &= ~FlHWChangedMouseFlag;
	    FlippedVideo = TRUE;
	} else
	    FlippedVideo = FALSE;
    }
    
    for (i=0; i<ScreenHeight*2; i++) {
	_start = start = (uldat)ChangedVideo[i>>1][i&1][0];
	_end   = end   = (uldat)ChangedVideo[i>>1][i&1][1];
	    
	if (start != (uldat)-1) {
		
	    /* actual tty size could be different from ScreenWidth*ScreenHeight... */
	    start += (i>>1) * ScreenWidth;
	    end   += (i>>1) * ScreenWidth;

	    _start += (i>>1) * HW->X;
	    _end   += (i>>1) * HW->X;
		
		
	    if (prevS != (uldat)-1) {
		if (start - prevE < HW->merge_Threshold) {
		    /* the two chunks are (almost) contiguous, merge them */
		    /* if HW->X != ScreenWidth we can merge only if they do not wrap */
		    if (HW->X == ScreenWidth || prevS / ScreenWidth == end / ScreenWidth) {
			_prevE = prevE = end;
			continue;
		    }
		}
		lseek(VcsaFd, 4+_prevS*sizeof(hwattr), SEEK_SET);
		write(VcsaFd, (void *)&Video[prevS], (prevE-prevS+1)*sizeof(hwattr));
	    }
	    prevS = start;
	    prevE = end;
	    _prevS = _start;
	    _prevE = _end;
	}
    }
    if (prevS != (uldat)-1) {
	lseek(VcsaFd, 4+_prevS*sizeof(hwattr), SEEK_SET);
	write(VcsaFd, (char *)&Video[prevS], (prevE-prevS+1)*sizeof(hwattr));
    }
    
    /* ... and this redraws the mouse */
    if (HW->FlagsHW & FlHWSoftMouse) {
	if (FlippedOldVideo)
	    OldVideo[HW->Last_x + HW->Last_y * ScreenWidth] = savedOldVideo;
	if (FlippedVideo)
	    VideoFlip(HW->Last_x = HW->MouseState.x, HW->Last_y = HW->MouseState.y);
	else if (HW->FlagsHW & FlHWChangedMouseFlag)
	    HW->ShowMouse();
    }
    
    /* now the cursor */
    
    if (CursorType != NOCURSOR && (CursorX != HW->XY[0] || CursorY != HW->XY[1])) {
	linux_MoveToXY(HW->XY[0] = CursorX, HW->XY[1] = CursorY);
	setFlush();
    }
    if (CursorType != HW->TT) {
	linux_SetCursorType(HW->TT = CursorType);
	setFlush();
    }

    HW->FlagsHW &= ~FlHWChangedMouseFlag;
}


/* HideMouse and ShowMouse depend on Video setup, not on Mouse.
 * so we have vcsa_ and stdout_ versions, not GPM_ ones... */
static void vcsa_ShowMouse(void) {
    uldat pos = (HW->Last_x = HW->MouseState.x) + (HW->Last_y = HW->MouseState.y) * ScreenWidth;
    uldat _pos = HW->Last_x + HW->Last_y * HW->X;
    
    hwattr h  = Video[pos];
    hwcol c = ~HWCOL(h) ^ COL(HIGH,HIGH);

    h = HWATTR( c, HWFONT(h) );

    lseek(VcsaFd, 4+_pos*sizeof(hwattr), SEEK_SET);
    write(VcsaFd, (char *)&h, sizeof(hwattr));
}

static void vcsa_HideMouse(void) {
    uldat pos = HW->Last_x + HW->Last_y * ScreenWidth;
    uldat _pos = HW->Last_x + HW->Last_y * HW->X;

    lseek(VcsaFd, 4+_pos*sizeof(hwattr), SEEK_SET);
    write(VcsaFd, (char *)&Video[pos], sizeof(hwattr));
}

#endif /* CONF_HW_TTY_LINUX */

/*
 * As alternate method, we also provide
 * output through stdout.
 * 
 * it is slower (?), but in these days it shouldn't be a problem.
 * 
 * this is used both to run inside a twin terminal,
 * and as fallback (if vcsa_InitVideo() fails) to run on a Linux console.
 */

static void linux_QuitVideo(void);
static void linux_ShowMouse(void);
static void linux_HideMouse(void);
static void linux_FlushVideo(void);

/* return FALSE if failed */
static byte linux_InitVideo(void) {
    byte *term = tty_TERM;
    
    if (!term) {
	fputs("      linux_InitVideo() failed: unknown terminal type.\n", stderr);
	return FALSE;
    }
    
#if 0
    /* we now have -hw=tty,termcap */
    do if (strcmp(term, "linux")) {
	if (!strncmp(term, "xterm", 5) || !strncmp(term, "rxvt", 4)) {
	    byte c;
	    
	    fprintf(stderr, "\n"
		    "      \033[1m  WARNING: terminal `%s' is not fully supported.\033[0m\n"
		    "\n"
		    "      If you really want to run `twin' on this terminal\n"
		    "      hit RETURN to continue, otherwise hit CTRL-C to quit now.\n", term);
	    fflush(stderr);
    
	    read(tty_fd, &c, 1);
	    if (c == '\n' || c == '\r')
		break;
	}
	fprintf(stderr, "      linux_InitVideo() failed: terminal type `%s' not supported.\n", term);
	return FALSE;
    } while (0);
#endif
    
    if (strcmp(term, "linux")) {
	fprintf(stderr, "      linux_InitVideo() failed: terminal `%s' is not `linux'.\n", term);
	return FALSE;
    }
    
    fputs("\033[0;11m\033[2J\033[3h", stdOUT); /* clear colors, clear screen, */
					       /* set IBMPC consolemap, set TTY_DISPCTRL */
    
    HW->FlushVideo = linux_FlushVideo;
    HW->FlushHW = stdout_FlushHW;

    HW->ShowMouse = linux_ShowMouse;
    HW->HideMouse = linux_HideMouse;
    HW->UpdateMouseAndCursor = linux_UpdateMouseAndCursor;
    
    HW->DetectSize  = stdin_DetectSize;
    HW->CheckResize = stdin_CheckResize;
    HW->Resize      = stdin_Resize;
    
    HW->HWSelectionImport  = AlwaysFalse;
    HW->HWSelectionExport  = NoOp;
    HW->HWSelectionRequest = (void *)NoOp;
    HW->HWSelectionNotify  = (void *)NoOp;
    HW->HWSelectionPrivate = NULL;

    HW->CanDragArea = linux_CanDragArea;
    HW->DragArea = linux_DragArea;
   
    HW->XY[0] = HW->XY[1] = 0;
    HW->TT = -1; /* force updating the cursor */
	
    HW->Beep = linux_Beep;
    HW->Configure = linux_Configure;
    HW->SetPalette = linux_SetPalette;
    HW->ResetPalette = linux_ResetPalette;

    HW->QuitVideo = linux_QuitVideo;
    
    HW->FlagsHW |= FlHWNeedOldVideo;
    HW->FlagsHW &= ~FlHWExpensiveFlushVideo;
    HW->NeedHW = 0;
    HW->merge_Threshold = 0;

    return TRUE;
}

static void linux_QuitVideo(void) {
    linux_MoveToXY(0, ScreenHeight-1);
    linux_SetCursorType(LINECURSOR);
    fputs("\033[0;10m\033[3l\n", stdOUT); /* restore original colors, consolemap and TTY_DISPCTRL */
    
    HW->QuitVideo = NoOp;
}


#define CTRL_ALWAYS 0x0800f501	/* Cannot be overridden by TTY_DISPCTRL */

#define linux_MogrifyInit() fputs("\033[0m", stdOUT); _col = COL(WHITE,BLACK)
#define linux_MogrifyNoCursor() fputs("\033[?25l", stdOUT);
#define linux_MogrifyYesCursor() fputs("\033[?25h", stdOUT);

INLINE void linux_SetColor(hwcol col) {
    static byte colbuf[] = "\033[2x;2x;4x;3xm";
    byte c, *colp = colbuf+2;
    
    if ((col & COL(HIGH,0)) != (_col & COL(HIGH,0))) {
	if (_col & COL(HIGH,0)) *colp++ = '2';
	*colp++ = '1'; *colp++ = ';';
    }
    if ((col & COL(0,HIGH)) != (_col & COL(0,HIGH))) {
	if (_col & COL(0,HIGH)) *colp++ = '2';
	*colp++ = '5'; *colp++ = ';';
    }
    if ((col & COL(0,WHITE)) != (_col & COL(0,WHITE))) {
	c = COLBG(col) & ~HIGH;
	*colp++ = '4'; *colp++ = VGA2ANSI(c) + '0'; *colp++ = ';';
    }
    if ((col & COL(WHITE,0)) != (_col & COL(WHITE,0))) {
	c = COLFG(col) & ~HIGH;
	*colp++ = '3'; *colp++ = VGA2ANSI(c) + '0'; *colp++ = ';';
    }
    _col = col;
    
    if (colp[-1] == ';') --colp;
    *colp++ = 'm'; *colp   = '\0';
    
    fputs(colbuf, stdOUT);
}

INLINE void linux_Mogrify(dat x, dat y, uldat len) {
    hwattr *V, *oV;
    hwcol col;
    byte c, sending = FALSE;
    
    V = Video + x + y * ScreenWidth;
    oV = OldVideo + x + y * ScreenWidth;
	
    for (; len; V++, oV++, x++, len--) {
	if (!ValidOldVideo || *V != *oV) {
	    if (!sending)
		sending = TRUE, linux_MoveToXY(x,y);

	    col = HWCOL(*V);
	    
	    if (col != _col)
		linux_SetColor(col);
	
	    c = HWFONT(*V);
	    if ((c < 32 && ((CTRL_ALWAYS >> c) & 1)) || c == 128+27)
		putc(' ', stdOUT); /* can't display it */
	    else
		putc(c, stdOUT);
	} else
	    sending = FALSE;
    }
}

INLINE void linux_SingleMogrify(dat x, dat y, hwattr V) {
    byte c;
    
    linux_MoveToXY(x,y);

    if (HWCOL(V) != _col)
	linux_SetColor(HWCOL(V));
	
    c = HWFONT(V);
    if ((c < 32 && ((CTRL_ALWAYS >> c) & 1)) || c == 128+27)
	putc(' ', stdOUT); /* can't display it */
    else
	putc(c, stdOUT);
}

/* HideMouse and ShowMouse depend on Video setup, not on Mouse.
 * so we have vcsa_ and linux_ versions, not GPM_ ones... */
static void linux_ShowMouse(void) {
    uldat pos = (HW->Last_x = HW->MouseState.x) + (HW->Last_y = HW->MouseState.y) * ScreenWidth;
    hwattr h  = Video[pos];
    hwcol c = ~HWCOL(h) ^ COL(HIGH,HIGH);

    linux_SingleMogrify(HW->MouseState.x, HW->MouseState.y, HWATTR( c, HWFONT(h) ));

    /* put the cursor back in place */
    HW->XY[0] = HW->XY[1] = (udat)-1;
    setFlush();
}

static void linux_HideMouse(void) {
    uldat pos = HW->Last_x + HW->Last_y * ScreenWidth;

    linux_SingleMogrify(HW->Last_x, HW->Last_y, Video[pos]);

    /* put the cursor back in place */
    HW->XY[0] = HW->XY[1] = (udat)-1;
    setFlush();
}

static void linux_UpdateCursor(void) {
    if ((CursorX != HW->XY[0] || CursorY != HW->XY[1]) && (CursorType != NOCURSOR)) {
	linux_MoveToXY(HW->XY[0] = CursorX, HW->XY[1] = CursorY);
	setFlush();
    }
    if (CursorType != HW->TT) {
	linux_SetCursorType(HW->TT = CursorType);
	setFlush();
    }
}

static void linux_UpdateMouseAndCursor(void) {
    if ((HW->FlagsHW & FlHWSoftMouse) && (HW->FlagsHW & FlHWChangedMouseFlag)) {
	HW->HideMouse();
	HW->ShowMouse();
	HW->FlagsHW &= ~FlHWChangedMouseFlag;
    }

    linux_UpdateCursor();
}

static void linux_FlushVideo(void) {
    dat i, j;
    dat start, end;
    byte FlippedVideo = FALSE, FlippedOldVideo = FALSE;
    hwattr savedOldVideo;
    
    if (!ChangedVideoFlag) {
	HW->UpdateMouseAndCursor();
	return;
    }

    /* hide the mouse if needed */
    
    /* first, check the old mouse position */
    if (HW->FlagsHW & FlHWSoftMouse) {
	if (HW->FlagsHW & FlHWChangedMouseFlag) {
	    /* dirty the old mouse position, so that it will be overwritten */
	    
	    /*
	     * with multi-display this is a hack, but since OldVideo gets restored
	     * below *BEFORE* returning from linux_FlushVideo(), that's ok.
	     */
	    DirtyVideo(HW->Last_x, HW->Last_y, HW->Last_x, HW->Last_y);
	    if (ValidOldVideo) {
		FlippedOldVideo = TRUE;
		savedOldVideo = OldVideo[HW->Last_x + HW->Last_y * ScreenWidth];
		OldVideo[HW->Last_x + HW->Last_y * ScreenWidth] = ~Video[HW->Last_x + HW->Last_y * ScreenWidth];
	    }
	}
	
        i = HW->MouseState.x;
	j = HW->MouseState.y;
	/*
	 * instead of calling ShowMouse(),
	 * we flip the new mouse position in Video[] and dirty it if necessary.
	 */
	if ((HW->FlagsHW & FlHWChangedMouseFlag) || (FlippedVideo = Plain_isDirtyVideo(i, j))) {
	    VideoFlip(i, j);
	    if (!FlippedVideo)
		DirtyVideo(i, j, i, j);
	    HW->FlagsHW &= ~FlHWChangedMouseFlag;
	    FlippedVideo = TRUE;
	} else
	    FlippedVideo = FALSE;
    }

    linux_MogrifyInit();
    linux_MogrifyNoCursor();
    for (i=0; i<ScreenHeight*2; i++) {
	start = ChangedVideo[i>>1][i&1][0];
	end   = ChangedVideo[i>>1][i&1][1];
	
	if (start != -1)
	    linux_Mogrify(start, i>>1, end-start+1);
    }
    /* put the cursor back in place */
    linux_MogrifyYesCursor();
    
    HW->XY[0] = HW->XY[1] = -1;
    
    setFlush();
    
    /* ... and this redraws the mouse */
    if (HW->FlagsHW & FlHWSoftMouse) {
	if (FlippedOldVideo)
	    OldVideo[HW->Last_x + HW->Last_y * ScreenWidth] = savedOldVideo;
	if (FlippedVideo)
	    VideoFlip(HW->Last_x = HW->MouseState.x, HW->Last_y = HW->MouseState.y);
	else if (HW->FlagsHW & FlHWChangedMouseFlag)
	    HW->ShowMouse();
    }
    
    linux_UpdateCursor();
    
    HW->FlagsHW &= ~FlHWChangedMouseFlag;
}

static void linux_Beep(void) {
    fputs("\033[3l\007\033[3h", stdOUT);
    setFlush();
}

static void linux_Configure(udat resource, byte todefault, udat value) {
    switch (resource) {
      case HW_KBDAPPLIC:
	fputs(todefault || !value ? "\033>" : "\033=", stdOUT);
	setFlush();
	break;
      case HW_ALTCURSKEYS:
	fputs(todefault || !value ? "\033[?1l" : "\033[?1h", stdOUT);
	setFlush();
	break;
      case HW_BELLPITCH:
	if (todefault)
	    fputs("\033[10]", stdOUT);
	else
	    fprintf(stdOUT, "\033[10;%hd]", value);
	setFlush();
	break;
      case HW_BELLDURATION:
	if (todefault)
	    fputs("\033[11]", stdOUT);
	else
	    fprintf(stdOUT, "\033[11;%hd]", value);
	setFlush();
	break;
      default:
	break;
    }
}

static void linux_SetPalette(udat N, udat R, udat G, udat B) {
    fprintf(stdOUT, "\033]P%1hx%02hx%02hx%02hx", N, R, G, B);
    setFlush();
}

static void linux_ResetPalette(void) {
    fputs("\033]R", stdOUT);
    setFlush();
}

static byte linux_CanDragArea(dat Left, dat Up, dat Rgt, dat Dwn, dat DstLeft, dat DstUp) {
    return Left == 0 && Rgt == HW->X-1 && Dwn == HW->Y-1 && DstUp == 0;
}

static void linux_DragArea(dat Left, dat Up, dat Rgt, dat Dwn, dat DstLeft, dat DstUp) {
    udat delta = Up - DstUp;
	
    HW->HideMouse();
    HW->FlagsHW |= FlHWChangedMouseFlag;
	
    fprintf(stdOUT,
	    "\033[?1c\033[0m"	/* hide cursor, reset color */
	    "\033[%d;1H", HW->Y);/* go to last line */
	    
    while (delta--)
	putc('\n', stdOUT);
    
    if (HW->FlushVideo == linux_FlushVideo)
	setFlush();
    else
	fflush(stdOUT);
	
    /* this will restore the cursor */
    HW->TT = -1;
    
    /*
     * now the last trick: tty scroll erased the part
     * below DstUp + (Dwn - Up) so we must redraw it.
     */
    NeedRedrawVideo(0, DstUp + (Dwn - Up) + 1, HW->X - 1, HW->Y - 1);
}

#endif /* defined(CONF_HW_TTY_LINUX) || defined(CONF_HW_TTY_TWTERM) */