/*
 *  libTw.c  --  implementation of libTw functions
 *
 *  Copyright (C) 1999-2001 by Massimiliano Ghilardi
 *
 * AVL (Adelson-Velskii and Landis) tree to speed up listener search
 * from O(n) to O(log n) based on code from linux kernel mm subsystem,
 * written by Bruno Haible <haible@ma2s2.mathematik.uni-karlsruhe.de>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 * 
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 *
 */

/*
 * Life is tricky... under SunOS hstrerror() is in an obscure library, so it gets disabled,
 * yet <netdb.h> has its prototype, so the #define hstrerror() in "Tw/missing.h" breaks it.
 * Solution: include "Tw/Tw.h" (pulls in "Tw/missing.h") late, but still include
 * "Tw/Twautoconf.h" and "Tw/osincludes.h" early to pull in TW_HAVE_* and system headers
 * necessary to include <sys/socket.h> under FreeBSD.
 */
#include "Tw/Twautoconf.h"
#include "Tw/osincludes.h"

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>

#ifdef CONF_SOCKET_GZ
# include <zlib.h>
#endif

#ifdef CONF_SOCKET_PTHREADS
# include <pthread.h>
#endif

#ifdef TW_HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif

#include "Tw/Tw.h"

#include "Tw/Twstat.h"
#include "Tw/Twerrno.h"
#include "Tw/Twavl.h"

#include "mutex.h"

#include "unaligned.h"
#include "md5.h"
#include "version.h"

/* remove the `Obj' suffix from Tw_ChangeFieldObj() */
#define Tw_ChangeFieldObj Tw_ChangeField

/* early check libTw.h against sockproto.h */
#include "paranoiam4.h"


#define Min2(a,b) ((a) < (b) ? (a) : (b))

#ifdef CONF_SOCKET_PTHREADS

typedef struct s_tw_errno {
    uldat E;
    uldat S;
    tw_self T;
} s_tw_errno;

typedef struct s_tw_errno_vec {
    s_tw_errno *vec;
    uldat max, last;
} s_tw_errno_vec;

#else

/* use libTwerrno.h types */
typedef tw_errno s_tw_errno;
typedef tw_errno s_tw_errno_vec;

#endif

typedef struct s_fn_list {
    void *Fn;
    byte len, formatlen;
    byte *name, *format;
} fn_list;

static fn_list Functions[] = {

#include "libTw1m4.h"
    
    {Tw_Stat, 0, 0, "Tw_StatObj", "0S0x"magic_id_STR(obj)"_"TWS_udat_STR"V"TWS_udat_STR },
    
    {NULL, 0, 0, NULL, NULL }
};

typedef uldat v_id_vec [ sizeof(Functions) / sizeof(Functions[0]) ];

/*
 * automagically get the symbols order_* to be the index
 * of corresponding element in Functions[] :
 * enums start at 0 and automatically increase...
 */
typedef enum e_fn_order {
    order_DoesNotExist = -1,
#   define EL(funct) order_##funct,
#   include "socklistm4.h"
#   undef EL
	
    order_StatObj,

} fn_order;

static void InitFunctions(void) {
    fn_list *f = Functions;
    if (f->len == 0) while (f->name) {
	f->len = Tw_LenStr(3 + f->name); /* skip "Tw_" */
	f->formatlen = Tw_LenStr(1 + f->format); /* skip "<self>" */
	f++;
    }
}


struct s_tlistener {
    tlistener Left, Right, AVLParent;
    uldat AVLkey;
    byte AVLHeight;
    uldat Type;
    tevent_any Event;
    tfn_listener Listener;
    void *Arg;
    tdisplay TwD;
};

#define QREAD    0
#define QWRITE   1
#define QMSG     2
#define QgzREAD  3
#define QgzWRITE 4
#define QMAX	 5

typedef struct s_tw_d {
#ifdef tw_mutex
    tw_mutex mutex;
#endif
    
    byte *Queue[5];
    uldat Qstart[5], Qlen[5], Qmax[5];
    /*
     * since DeQueue(Q[gz]WRITE) cancels bytes from end, not from start
     * 
     * Qstart[QWRITE] is used only if Tw_TimidFlush()
     * fails writing the whole queue,
     * while Qstart[QgzWRITE] is not used at all.
     */
    
    uldat *r;
    byte *s;

    int Fd;
    uldat RequestN;

    tlistener AVLRoot;
    tfn_default_listener DefaultListener;
    void *DefaultArg;

    s_tw_errno_vec rErrno_;

    byte ServProtocol[3];
    byte PanicFlag;

#ifdef CONF_SOCKET_GZ
    byte GzipFlag;
    z_streamp zR, zW;
#endif

    v_id_vec id_vec;
    
} *tw_d;

#define rErrno	(TwD->rErrno_)
#define mutex	(TwD->mutex)
#define id_Tw	(TwD->id_vec)
#define Queue	(TwD->Queue)
#define Qstart	(TwD->Qstart)
#define Qlen	(TwD->Qlen)
#define Qmax	(TwD->Qmax)
#define r	(TwD->r)
#define s	(TwD->s)
#define Fd	(TwD->Fd)
#define RequestN  (TwD->RequestN)
#define ServProtocol (TwD->ServProtocol)
#define PanicFlag (TwD->PanicFlag)
#define GzipFlag  (TwD->GzipFlag)
#define zR	(TwD->zR)
#define zW	(TwD->zW)


#define LOCK tw_mutex_lock(mutex)
#define UNLK tw_mutex_unlock(mutex)
#define NO_LOCK do { } while (0)
#define NO_UNLK do { } while (0)

static s_tw_errno rCommonErrno;
#define CommonErrno (rCommonErrno.E)

static uldat OpenCount;
#ifdef tw_mutex
static tw_mutex OpenCountMutex = TW_MUTEX_INITIALIZER;
#endif

/* and this is the 'default' display */
tw_d Tw_DefaultD;

#ifdef CONF_SOCKET_GZ
static uldat Gzip(tw_d TwD);
static uldat Gunzip(tw_d TwD);
#endif

static byte Sync(tw_d TwD);
static void Panic(tw_d TwD);
static void ParseReplies(tw_d TwD);

byte Tw_EnableGzip(tw_d TwD);

void *(*Tw_AllocMem)(size_t) = malloc;
void *(*Tw_ReAllocMem)(void *, size_t) = realloc;
void  (*Tw_FreeMem)(void *) = free;

/**
 * creates a copy of a chunk of memory
 */
void *Tw_CloneMem(TW_CONST void *S, size_t len) {
    void *T;
    if (S && (T = Tw_AllocMem(len)))
	return Tw_CopyMem(S, T, len);
  return NULL;
}
/**
 * creates a copy of a null-terminated string string
 */
byte *Tw_CloneStr(TW_CONST byte *S) {
    size_t len;
    byte *T;
    if (S) {
	len = 1 + Tw_LenStr(S);
	if ((T = Tw_AllocMem(len)))
	    return Tw_CopyMem(S, T, len);
    }
    return NULL;
}

/**
 * sets the function to call to allocate/realloc/free memory;
 * can be called only if no connections to server are open.
 */
void Tw_ConfigMalloc(void *(*my_malloc)(size_t),
		     void *(*my_realloc)(void *, size_t),
		     void  (*my_free)(void *)) {
    
    tw_mutex_lock(OpenCountMutex);
    if (!OpenCount) {
	if (my_malloc && my_realloc && my_free) {
	    Tw_AllocMem = my_malloc;
	    Tw_ReAllocMem = my_realloc;
	    Tw_FreeMem = my_free;
	} else {
	    Tw_AllocMem = malloc;
	    Tw_ReAllocMem = realloc;
	    Tw_FreeMem = free;
	}
    }
    tw_mutex_unlock(OpenCountMutex);
}

#ifdef CONF_SOCKET_PTHREADS

TW_INLINE byte GrowErrnoLocation(tw_d TwD) {
    s_tw_errno *vec;
    uldat newmax = rErrno.max <= 8 ? 16 : (rErrno.max<<1);
    
    if ((vec = (s_tw_errno *)Tw_ReAllocMem(rErrno.vec, newmax * sizeof(s_tw_errno)))) {

	/* assume (tw_self)-1 is _NOT_ a valid thread identifier */
	Tw_WriteMem(vec + rErrno.max, '\xFF', (newmax-rErrno.max) * sizeof(s_tw_errno));
	rErrno.vec = vec;
	rErrno.max = newmax;
	
	return TRUE;
    }
    /* out of memory! */
    return FALSE;
}

static s_tw_errno *GetErrnoLocation(tw_d TwD) {
    tw_self self;
    uldat i;
    if (TwD) {
	self = tw_self_get();
	
	/* we cache last thread that called GetErrnoLocation() */
	i = rErrno.last;
	if (i < rErrno.max && rErrno.vec[i].T == self)
	    return &rErrno.vec[i];
	
	for (i=0; i<rErrno.max; i++) {
	    if (rErrno.vec[i].T == self)
		break;
	    if (rErrno.vec[i].T == (tw_self)-1) {
		/* empty slot, initialize it */
		rErrno.vec[i].T = self;
		rErrno.vec[i].E = rErrno.vec[i].S = 0;
		break;
	    }
	}
	if (i < rErrno.max) {
	    rErrno.last = i;
	    return &rErrno.vec[i];
	}
	if (GrowErrnoLocation(TwD)) {
	    rErrno.vec[i].E = rErrno.vec[i].S = 0;
	    rErrno.vec[i].T = self;
	    rErrno.last = i; /* i is previous rErrno.max */
	    return &rErrno.vec[i];
	}
    }
    return &rCommonErrno;
}

static void FreeErrnoLocation(tw_d TwD) {
    if (rErrno.vec)
	Tw_FreeMem(rErrno.vec);
}

# define Errno (GetErrnoLocation(TwD)->E)

#else /* !CONF_SOCKET_PTHREADS */

# define GetErrnoLocation(TwD)	((TwD) ? &TwD->rErrno_ : &rCommonErrno)
# define Errno			(GetErrnoLocation(TwD)->E)
# define FreeErrnoLocation(TwD)	do { } while (0)

#endif /* CONF_SOCKET_PTHREADS */


static uldat AddQueue(tw_d TwD, byte i, uldat len, void *data) {
    uldat nmax;
    byte *t;
    
    if (len == 0)
	return len;
    
    /* append to queue */
    if (Qstart[i] + Qlen[i] + len > Qmax[i]) {
	if (Qstart[i]) {
	    Tw_MoveMem(Queue[i] + Qstart[i], Queue[i], Qlen[i]);
	    Qstart[i] = 0;
	}
	if (Qlen[i] + len > Qmax[i]) {
	    t = (byte *)Tw_ReAllocMem(Queue[i], nmax = (Qmax[i]+len+40)*5/4);
	    if (!t)
		return 0;
	    Queue[i] = t;
	    Qmax[i] = nmax;
	}
    }
    if (data)
	Tw_CopyMem(data, Queue[i] + Qstart[i] + Qlen[i], len);
    Qlen[i] += len;
    return len;
}

TW_INLINE byte *GetQueue(tw_d TwD, byte i, uldat *len) {
    if (len) *len = Qlen[i];
    return Queue[i] + Qstart[i];
}

/* add data to a queue keeping it aligned at 8 bytes (for tmsgs) */
TW_INLINE uldat ParanoidAddQueueQMSG(tw_d TwD, uldat len, void *data) {
    byte *t = data + 2 * sizeof(uldat);
    uldat mtype, minlen, xlen;
    tmsg M;
    tevent_any E;
    
    /* we already checked (len >= 3 * sizeof(uldat)) */
    Pop(t,uldat,mtype);
    switch (mtype &= TW_MAXUDAT) {
      case TW_MSG_DISPLAY:
	minlen = sizeof(struct s_tevent_display) - sizeof(uldat);
	break;
      case TW_MSG_WIDGET_KEY:
	minlen = sizeof(struct s_tevent_keyboard);
	break;
      case TW_MSG_WIDGET_MOUSE:
	minlen = sizeof(struct s_tevent_mouse);
	break;
      case TW_MSG_WIDGET_CHANGE:
	minlen = sizeof(struct s_tevent_widget);
	break;
      case TW_MSG_WIDGET_GADGET:
	minlen = sizeof(struct s_tevent_gadget);
	break;
      case TW_MSG_MENU_ROW:
	minlen = sizeof(struct s_tevent_menu);
	break;
      case TW_MSG_SELECTION:
      case TW_MSG_SELECTIONCLEAR:
	minlen = sizeof(struct s_tevent_selection);
	break;
      case TW_MSG_SELECTIONNOTIFY:
	minlen = (sizeof(struct s_tevent_selectionnotify) - 1) & ~(sizeof(uldat) - 1);
	break;
      case TW_MSG_SELECTIONREQUEST:
	minlen = sizeof(struct s_tevent_selectionrequest);
	break;
      case TW_MSG_USER_CONTROL:
	minlen = sizeof(struct s_tevent_control) - sizeof(uldat) + 1;
	break;
      case TW_MSG_USER_CLIENTMSG:
	minlen = sizeof(struct s_tevent_clientmsg) - sizeof(uldat);
	break;
      default:
	return 0;
    }
    minlen += sizeof(struct s_tmsg) - sizeof(union s_tevent_any);
    
    if (len >= minlen && AddQueue(TwD, QMSG, len, data)) {
	if ((len & 7) && !AddQueue(TwD, QMSG, 8 - (len & 7), NULL)) {
	    Qlen[QMSG] -= len;
	    return 0;
	}
	/* check variable-length messages: */
	M = (tmsg)GetQueue(TwD, QMSG, NULL);
	E = &M->Event;
	
	switch (mtype &= TW_MAXUDAT) {
	  case TW_MSG_DISPLAY:
	    xlen = E->EventDisplay.Len;
	    break;
	  case TW_MSG_WIDGET_KEY:
	    xlen = E->EventKeyboard.SeqLen;
	    break;
	  case TW_MSG_SELECTIONNOTIFY:
	    xlen = E->EventSelectionNotify.Len;
	    break;
	  case TW_MSG_USER_CONTROL:
	    xlen = E->EventControl.Len;
	    break;
	  case TW_MSG_USER_CLIENTMSG:
	    xlen = E->EventClientMsg.Len;
	    break;
	  default:
	    xlen = 0;
	    break;
	}
	len = (len + 7) & ~7;
	if (M->Len == xlen + minlen)
	    return len;
	Qlen[QMSG] -= len;
    }
    return 0;
}


#define QLeft(Q,len)	(Qlen[Q] + Qstart[Q] + (len) <= Qmax[Q] ? Qlen[Q] += (len) : Grow(TwD, Q, len))
#define RQLeft(len)     QLeft(QREAD,len)
#define WQLeft(len)     QLeft(QWRITE,len)


static uldat Grow(tw_d TwD, byte i, uldat len) {
    /* make enough space available in Queue[i] and mark it used */
    uldat nmax;
    byte *t;
    
    if ((i == QREAD || i == QgzREAD) && Qlen[i] + len < Qmax[i]) {
	Tw_MoveMem(Queue[i] + Qstart[i], Queue[i], Qlen[i]);
	Qstart[i] = 0;
    } else {
	t = (byte *)Tw_ReAllocMem(Queue[i], nmax = (Qmax[i]+len+40)*5/4);
	if (!t)
	    return 0;
	if (i == QWRITE) {
	    r = (uldat *)(t + ((byte *)r - Queue[i]));
	    s = t + (s - Queue[i]);
	}
	Queue[i] = t;
	Qmax[i] = nmax;
    }
    return Qlen[i] += len;
}

static uldat *InitRS(tw_d TwD) {
    uldat len;
    if (WQLeft(3*sizeof(uldat))) {
	s = GetQueue(TwD, QWRITE, &len);
	s += len;
	return r = (uldat *)s - 3;
    }
    Errno = TW_ENO_MEM;
    return (uldat *)0;
}

TW_INLINE uldat DeQueue(tw_d TwD, byte i, uldat len) {
    if (!len)
	return len;
    
    switch (i) {
      case QREAD:
      case QMSG:
      case QgzREAD:
	/* QREAD, QMSG: DeQueue() from start (FIFO like) */
	if (len < Qlen[i]) {
	    Qstart[i] += len;
	    Qlen[i] -= len;
	} else {
	    len = Qlen[i];
	    Qstart[i] = Qlen[i] = 0;
	}
	return len;
      case QWRITE:
      case QgzWRITE:
	/* QWRITE: DeQueue() from end (stack like) */

	if (len < Qlen[i]) {
	    Qlen[i] -= len;
	} else {
	    len = Qlen[i];
	    Qlen[i] = 0;
	}
	return len;
      default:
	return (uldat)0;
    }
}

#define DeQueueAligned(TwD, i, len)	DeQueue(TwD, i, ((len) + 7) & ~7)



/* remove the given reply from QREAD */
static void KillReply(tw_d TwD, byte *rt, uldat rlen) {
    byte *t;
    uldat len;
    
    rlen += sizeof(uldat);
    t = GetQueue(TwD, QREAD, &len);
    if (rt >= t && rt + rlen <= t + len) {
	if (rt == t)
	    /* same as Qstart[QREAD] = Qlen[QREAD] = 0; */
	    DeQueue(TwD, QREAD, rlen);
	else {
	    if (rt + rlen < t + len) {
		uldat before = rt - t;
		uldat after = t + len - (rt + rlen);
		/* which Tw_MoveMem() copies less data ? */
		if (before <= after) {
		    Tw_MoveMem(t, t + rlen, before);
		    Qstart[QREAD] += rlen;
		} else
		    Tw_MoveMem(rt + rlen, rt, after);
	    }
	    Qlen[QREAD] -= rlen;
	}
    }
}

static void Panic(tw_d TwD) {
    uldat len;
    
    (void)GetQueue(TwD, QREAD, &len);
    DeQueue(TwD, QREAD, len);

    (void)GetQueue(TwD, QWRITE, &len);
    DeQueue(TwD, QWRITE, len);

    (void)GetQueue(TwD, QMSG, &len);
    DeQueue(TwD, QMSG, len);

#ifdef CONF_SOCKET_GZ
    (void)GetQueue(TwD, QgzREAD, &len);
    DeQueue(TwD, QgzREAD, len);

    (void)GetQueue(TwD, QgzWRITE, &len);
    DeQueue(TwD, QgzWRITE, len);
#endif
    
    if (Fd >= 0) {
	close(Fd);
	Fd = TW_NOFD;
    }
    
    PanicFlag = TRUE;
}

/**
 * returns TRUE if a fatal error occurred, FALSE otherwise;
 * after a fatal error, the only useful thing to do is Tw_Close()
 */
byte Tw_InPanic(tw_d TwD) {
    return TwD && PanicFlag;
}

/* cancel the last request packet */
/* you can (must) call Fail() ONLY after a failed WQLeft() */
static void Fail(tw_d TwD) {
    DeQueue(TwD, QWRITE, s - (byte *)r);
}

static byte Flush(tw_d TwD, byte Wait) {
    fd_set fset;
    uldat chunk = 0, left;
    s_tw_errno *E;
    byte *t;
    byte Q;
    int fd;
    
    t = GetQueue(TwD, Q = QWRITE, &left);

    if (Fd != TW_NOFD && left) {

#ifdef CONF_SOCKET_GZ
	if (GzipFlag) {
	    if (Gzip(TwD)) {
		t = GetQueue(TwD, Q = QgzWRITE, &left);
	    } else
		return FALSE; /* TwGzip() calls Panic() if needed */
	}
#endif
	FD_ZERO(&fset);
	
	while (left > 0) {
	    chunk = write(Fd, t, left);
	    if (chunk && chunk != (uldat)-1) {
		/* would be "if (chunk > 0)" but chunk is unsigned */
		t += chunk;
		left -= chunk;		
		if (chunk < Qlen[Q]) {
		    Qstart[Q] += chunk;
		    Qlen[Q] -= chunk;
		} else
		    Qstart[Q] = Qlen[Q] = 0;
	    }
	    else if (chunk == (uldat)-1 && errno == EINTR)
		; /*continue*/
	    else if (chunk == (uldat)-1 && errno == EWOULDBLOCK && Wait) {
		do {
		    fd = Fd;
		    /* release the lock before sleeping!!! */
		    UNLK;
		    FD_SET(fd, &fset);
		    chunk = select(fd+1, NULL, &fset, NULL, NULL);
		    FD_CLR(fd, &fset);
		    LOCK;
		    /* maybe another thread did our work while we slept? */
		    t = GetQueue(TwD, Q, &left);
		    if (!left)
			break;
		} while (chunk == (uldat)-1 && errno == EINTR);
		
		if (chunk == (uldat)-1)
		    break;
	    } else
		break;
	}
    } else
	left = 0;
    
    if (left && Wait) {
	E = GetErrnoLocation(TwD);
	E->E = TW_ECANT_WRITE;
	E->S = errno;
	Panic(TwD);
    }
    return (Fd != TW_NOFD) + (Fd != TW_NOFD && !Wait && left);
}

/**
 * sends all buffered data to server, blocking
 * if not all data can be immediately sent
 */
byte Tw_Flush(tw_d TwD) {
    byte b;
    LOCK; b = Flush(TwD, TRUE); UNLK;
    return b;
}

/**
 * sends all buffered data to server, without blocking:
 * if not all data can be immediately sent, unsent data is kept in buffer
 */
byte Tw_TimidFlush(tw_d TwD) {
    byte b;
    LOCK; b = Flush(TwD, FALSE); UNLK;
    return b;
}
    
/* return bytes read, or (uldat)-1 for errors */
static uldat TryRead(tw_d TwD, byte Wait) {
    fd_set fset;
    uldat got = 0, len;
    int sel, fd;
    byte *t, mayread;
    byte Q;
    
#ifdef CONF_SOCKET_GZ
    if (GzipFlag)
	Q = QgzREAD;
    else
#endif
	Q = QREAD;
    
    if (Wait) {
	FD_ZERO(&fset);
	do {
	    fd = Fd;
	    /* drop LOCK before sleeping! */
	    UNLK;
	    FD_SET(fd, &fset);
	    sel = select(fd+1, &fset, NULL, NULL, NULL); 
	    FD_CLR(fd, &fset);
	    LOCK;

	    /* maybe another thread received some data? */
	    (void)GetQueue(TwD, QREAD, &len);
	    if (len)
		break;
	} while (sel != 1);
    }
    
    mayread = ioctl(Fd, FIONREAD, &len) >= 0;
    if (!mayread || !len)
	len = TW_SMALLBUFF;
    
    if (QLeft(Q,len)) {
	t = GetQueue(TwD, Q, &got);
	t += got - len;
	do {
	    got = read(Fd, t, len);
	} while (got == (uldat)-1 && errno == EINTR);
	
	Qlen[Q] -= len - (got == (uldat)-1 ? 0 : got);
	
	if (got == 0 || (got == (uldat)-1 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
	    Errno = TW_ELOST_CONN;
	    Panic(TwD);
	    return (uldat)-1;
	}
    }
    if (got == (uldat)-1)
	got = 0;
    
#ifdef CONF_SOCKET_GZ
    if (GzipFlag && got) {
	got = Gunzip(TwD);
    }
#endif
    return got;
}

TW_INLINE byte *FindReply(tw_d TwD, uldat Serial) {
    uldat left, rlen, rSerial;
    byte *rt, *t = GetQueue(TwD, QREAD, &left);
    
    /* sequentially examine all received replies */
    while (left >= 3*sizeof(uldat)) {
	rt = t;
	Pop(t,uldat,rlen);
	Pop(t,uldat,rSerial);
	/* it this reply complete? */
	if (left >= rlen + sizeof(uldat)) {
	    if (rSerial == Serial)
		return rt;
	    else {
		/* skip to next reply */
		t += rlen - sizeof(uldat);
		left -= rlen + sizeof(uldat);
	    }
	} else
	    /* last reply is incomplete, no more replies to examine */
	    break;
    }
    return NULL;
}


static byte *Wait4Reply(tw_d TwD, uldat Serial) {
    uldat left;
    byte *MyReply = NULL;
    if (Fd != TW_NOFD && ((void)GetQueue(TwD, QWRITE, &left), left)) {
	if (Flush(TwD, TRUE)) while (Fd != TW_NOFD && !(MyReply = FindReply(TwD, Serial)))
	    if (TryRead(TwD, TRUE) != (uldat)-1)
		ParseReplies(TwD);
    }
    return Fd != TW_NOFD ? MyReply : NULL;
}

static uldat ReadUldat(tw_d TwD) {
    uldat l, chunk;
    byte *t;
    
    (void)GetQueue(TwD, QREAD, &l);
    while (Fd != TW_NOFD && l < sizeof(uldat)) {
	if ((chunk = TryRead(TwD, TRUE)) != (uldat)-1)
	    l += chunk;
	else
	    return 0;
    }
	
    t = GetQueue(TwD, QREAD, NULL);
    Pop(t, uldat, l);
    DeQueue(TwD, QREAD, sizeof(uldat));
    return l;
}

#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')

static void ExtractServProtocol(tw_d TwD, byte *servdata, uldat len) {
    uldat i = 0;
    byte c;
    
    ServProtocol[0] = ServProtocol[1] = ServProtocol[2] = 0;
    
    while (len) {
	c = *servdata;
	while (len && (c = *servdata) && IS_DIGIT(c)) {
	    ServProtocol[i] *= 10;
	    ServProtocol[i] += c - '0';
	    --len;
	    ++servdata;
	}
	while (len && (c = *servdata) && !IS_DIGIT(c)) {
	    --len;
	    ++servdata;
	}
	if (++i >= 3)
	    break;
    }
}

/**
 * returns library protocol version
 */
uldat Tw_LibraryVersion(tw_d TwD) {
    return TW_PROTOCOL_VERSION;
}

/**
 * returns server protocol version
 */
uldat Tw_ServerVersion(tw_d TwD) {
    uldat l;
    LOCK;
    l = TWVER_BUILD(ServProtocol[0],ServProtocol[1],ServProtocol[2]);
    UNLK;
    return l;
}


static byte ProtocolNumbers(tw_d TwD) {
    byte *servdata, *hostdata = " Twin-" TW_STR(TW_PROTOCOL_VERSION_MAJOR) ".";
    uldat len = 0, chunk, _len = strlen(hostdata);
    
    while (Fd != TW_NOFD && (!len || ((servdata = GetQueue(TwD, QREAD, NULL)), len < *servdata))) {
	if ((chunk = TryRead(TwD, TRUE)) != (uldat)-1)
	    len += chunk;
    }

    if (Fd != TW_NOFD) {
	servdata = GetQueue(TwD, QREAD, &len);
    
	if (*servdata >= _len && !TwCmpMem(hostdata+1, servdata+1, _len-1)) {
	    ExtractServProtocol(TwD, servdata+6, *servdata-6);
	    DeQueue(TwD, QREAD, *servdata);
	    return TRUE;
	} else
	    Errno = TW_EX_PROTOCOL;
    }
    return FALSE;
}

TW_DECL_MAGIC(Tw_MagicData);

/**
 * returns TRUE if user-provided magic numbers
 * are compatible with library ones; used to avoid mixing
 * unicode clients with non-unicode library and vice-versa
 */
byte Tw_CheckMagic(TW_CONST byte id[])  {
    if (Tw_CmpMem(id+1, Tw_MagicData+1, (id[0] < Tw_MagicData[0] ? id[0] : Tw_MagicData[0]) - 2 - sizeof(uldat))) {
	CommonErrno = TW_EXLIB_SIZES;
	return FALSE;
    }
    return TRUE;
}

static byte MagicNumbers(tw_d TwD) {
    uldat len = 0, chunk = TWIN_MAGIC;
    byte *hostdata;
    
    Tw_CopyMem(&chunk, Tw_MagicData+Tw_MagicData[0]-sizeof(uldat), sizeof(uldat));
    
    /* send our magic to server */
    if (!AddQueue(TwD, QWRITE, Tw_MagicData[0], Tw_MagicData) || !Flush(TwD, TRUE))
	return FALSE;

    /* wait for server magic */
    while (Fd != TW_NOFD && (!len || ((hostdata = GetQueue(TwD, QREAD, NULL)), len < *hostdata))) {
	if ((chunk = TryRead(TwD, TRUE)) != (uldat)-1)
	    len += chunk;
    }

    /*
     * at the moment, no client-side datasize or endianity translation is available...
     * so just check against our magic
     */
    if (Fd != TW_NOFD) {
	hostdata = GetQueue(TwD, QREAD, &len);
    
	if (*hostdata > TWS_hwcol + sizeof(uldat) + 1 &&
	    /*
	     * allow server to send us fewer or more types than we sent,
	     * but ensure it agrees on the common ones.
	     */
	    !TwCmpMem(Tw_MagicData+1, hostdata+1,
		      Min2(*hostdata, Tw_MagicData[0]) - sizeof(uldat) - 2)) {
	    if (!TwCmpMem(Tw_MagicData + Tw_MagicData[0] - sizeof(uldat),
			  hostdata + *hostdata - sizeof(uldat), sizeof(uldat))) {
		DeQueue(TwD, QREAD, *hostdata);
		return TRUE;
	    } else
		Errno = TW_EX_ENDIAN;
	} else
	    Errno = TW_EX_SIZES;
    }
    return FALSE;
}

#define digestLen       16  /* hardcoded in MD5 routines */
#define hAuthLen	256 /* length of ~/.TwinAuth */
#define challengeLen	512 /* length of ~/.TwinAuth + random data */

static byte MagicChallenge(tw_d TwD) {
    struct MD5Context ctx;
    byte *t, *data, *home;
    uldat len, got, challenge, chunk;
    int fd;
    
    challenge = ReadUldat(TwD);
    if (Fd == TW_NOFD)
	return FALSE;
    if (challenge == TW_GO_MAGIC)
	return TRUE;
    if (challenge != TW_WAIT_MAGIC) {
	Errno = TW_ESTRANGE;
	return FALSE;
    }
    if (!(home = getenv("HOME"))) {
	Errno = TW_ENO_AUTH;
	return FALSE;
    }
    if (!WQLeft(digestLen) || !(data = Tw_AllocMem(hAuthLen))) {
	Errno = TW_ENO_MEM;
	return FALSE;
    }
	
    
    len = TwLenStr(home);
    if (len > hAuthLen - 11)
	len = hAuthLen - 11;
    
    Tw_CopyMem(home, data, len);
    Tw_CopyMem("/.TwinAuth", data+len, 11);
    if ((fd = open(data, O_RDONLY)) < 0) {
	Tw_FreeMem(data);
	Errno = TW_ENO_AUTH;
	return FALSE;
    }
    for (len = 0, got = 1; got && len < hAuthLen; len += got) {
	do {
	    got = read(fd, data + len, hAuthLen - len);
	} while (got == (uldat)-1 && errno == EINTR);
	if (got == (uldat)-1)
	    break;
    }
    close(fd);
    
    challenge = ReadUldat(TwD);
    if (Fd == TW_NOFD || got == (uldat)-1 || len + challenge != challengeLen) {
	Tw_FreeMem(data);
	if (Fd != TW_NOFD)
	    Errno = TW_ENO_AUTH;
	return FALSE;
    }
    
    (void)GetQueue(TwD, QREAD, &got);
    while (Fd != TW_NOFD && got < challenge) {
	if ((chunk = TryRead(TwD, TRUE)) != (uldat)-1)
	    got += chunk;
    }
    
    if (Fd == TW_NOFD)
	return FALSE;
    
    MD5Init(&ctx);
    MD5Update(&ctx, data, len);
    
    t = GetQueue(TwD, QREAD, NULL);
    MD5Update(&ctx, t, challenge);

    t = GetQueue(TwD, QWRITE, NULL); /* we did WQLeft(digestLen) above */
    MD5Final(t, &ctx);

    DeQueue(TwD, QREAD, challenge);
    
    Flush(TwD, TRUE);
    challenge = ReadUldat(TwD);
    
    if (challenge == TW_GO_MAGIC)
	return TRUE;
    if (Fd != TW_NOFD)
	Errno = TW_EDENIED;
    return FALSE;
}

/**
 * opens a connection to server; TwDisplay is the server to contact;
 * if NULL the environment variable $TWDISPLAY is used
 */
tw_d Tw_Open(TW_CONST byte *TwDisplay) {
    tw_d TwD;
    int result = -1, fd = TW_NOFD;
    byte *options, gzip = FALSE;

    if (Functions[order_FindFunction].len == 0)
	InitFunctions();
    
    if (!TwDisplay && (!(TwDisplay = getenv("TWDISPLAY")) || !*TwDisplay)) {
	CommonErrno = TW_ENO_DISPLAY;
	return (tw_d)0;
    }
    
    if ((options = strchr(TwDisplay, ','))) {
	*options = '\0';
	if (!TwCmpMem(options+1, "gz", 2))
	    gzip = TRUE;
    }

    CommonErrno = 0;

    if (*TwDisplay == ':') do {
	/* unix socket */
	struct sockaddr_un addr;
	
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
	    CommonErrno = TW_ENO_SOCKET;
	    break;
	}
	Tw_WriteMem(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	Tw_CopyStr("/tmp/.Twin", addr.sun_path);
	Tw_CopyStr(TwDisplay, addr.sun_path + 10);
	
	result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	
    } while(0); else do {
	/* inet socket */
	struct sockaddr_in addr;
	struct hostent *host_info;
	byte *server = Tw_CloneStr(TwDisplay), *p;
	unsigned short port;
	
	if (!server) {
	    CommonErrno = TW_ENO_MEM;
	    break;
	}
	    
	p = strchr(server, ':');
	
	if (!p) {
	    CommonErrno = TW_EBAD_DISPLAY;
	    Tw_FreeMem(server);
	    break;
	}
	*p = '\0';
	port = TW_INET_PORT + strtoul(p+1, NULL, 16);

	Tw_WriteMem(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	
	/* check if the server is a numbers-and-dots host like "127.0.0.1" */
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(server);

	if (addr.sin_addr.s_addr == (unsigned long)-1) {
#ifdef TW_HAVE_GETHOSTBYNAME
	    /* may be a FQDN host like "www.gnu.org" */
	    host_info = gethostbyname(server);
	    if (host_info) {
		Tw_CopyMem(host_info->h_addr, &addr.sin_addr, host_info->h_length);
		addr.sin_family = host_info->h_addrtype;
	    } else
#endif
	    {
		/* unknown hostname */
		rCommonErrno.E = TW_ENO_HOST;
		rCommonErrno.S = h_errno;
		Tw_FreeMem(server);
		break;
	    }
	}
	
	Tw_FreeMem(server);

	if ((fd = socket(addr.sin_family, SOCK_STREAM, 0)) >= 0)
	    result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	else
	    CommonErrno = TW_ENO_SOCKET;
	    
    } while (0);

    if (options)
	*options = ',';

    if (result == -1) { /* some error occurred */
	if (fd != TW_NOFD) {
	    close(fd);
	    CommonErrno = TW_ECANT_CONN;
	}
	/* try to get a meaningful message for the error */
	if (CommonErrno == TW_ENO_SOCKET || CommonErrno == TW_ECANT_CONN)
	    rCommonErrno.S = errno;
	return (tw_d)0;
    }

    if (!(TwD = (tw_d)Tw_AllocMem(sizeof(struct s_tw_d)))) {
	close(fd);
	CommonErrno = TW_ENO_MEM;
	return (tw_d)0;
    }

    Tw_WriteMem(TwD, 0, sizeof(struct s_tw_d));
    tw_mutex_init(mutex);
    Fd = fd;

    fcntl(Fd, F_SETFD, FD_CLOEXEC);
    fcntl(Fd, F_SETFL, O_NONBLOCK);
    
    tw_mutex_lock(OpenCountMutex);
    OpenCount++;
    tw_mutex_unlock(OpenCountMutex);

    LOCK;
    if (ProtocolNumbers(TwD) && MagicNumbers(TwD) && MagicChallenge(TwD)) {
	UNLK;
	if (gzip)
	    (void)Tw_EnableGzip(TwD);
	return TwD;
    }
    UNLK;
    
    close(Fd); Fd = TW_NOFD; /* to skip Sync() */
    Tw_Close(TwD);
    return (tw_d)0;
}

/* just like all Tw_* functions, this requires LOCK not to be held */
/*
 * Tw_Close() is not completely thread-safe, but it would be useless anyway
 * for it to be thread safe, since after Tw_Close() the TwD pointer is invalid
 * and the only way for other threads to know that is to cooperate with the one
 * that issued Tw_Close().
 */
/**
 * closes a server connection
 */
void Tw_Close(tw_d TwD) {
    static void DeleteAllListeners(tlistener);
    s_tw_errno *E;
    byte *q;
    int i;

    if (!TwD)
	return;

    LOCK;
    
    if (Fd != TW_NOFD) {
	Sync(TwD);
	close(Fd);
    }
#ifdef CONF_SOCKET_GZ
    if (GzipFlag)
	Tw_DisableGzip(TwD);
#endif
    for (i = 0; i < QMAX; i++) {
	if ((q = Queue[i]))
	    Tw_FreeMem(q);
    }
    
    /* save Errno in CommonErrno */
    E = GetErrnoLocation(TwD);
    rCommonErrno.E = E->E;
    rCommonErrno.S = E->S;
    
    DeleteAllListeners(TwD->AVLRoot);
    
    /*PanicFlag = FALSE;*/
    UNLK;
    tw_mutex_destroy(mutex);
    
    FreeErrnoLocation(TwD);
    Tw_FreeMem(TwD);
    
    tw_mutex_lock(OpenCountMutex);
    OpenCount--;
    tw_mutex_unlock(OpenCountMutex);
}


/*
 * Tw_AttachGetReply() returns 0 for failure, 1 for success,
 * else message string (len) bytes long.
 * 
 * it bypasses any compression.
 */
/* this requires LOCK not to be held */
/**
 * returns one of the messages produced by server after Tw_Attach()
 */
TW_CONST byte *Tw_AttachGetReply(tw_d TwD, uldat *len) {
    uldat chunk;
    byte *answ = (byte *)-1, *nul;
#ifdef CONF_SOCKET_GZ
    byte wasGzipFlag;
#endif
    
    LOCK;
    
#ifdef CONF_SOCKET_GZ
    wasGzipFlag = GzipFlag;
    GzipFlag = FALSE;
#endif
    
    if (Fd != TW_NOFD) do {
	
	answ = GetQueue(TwD, QREAD, &chunk);
	if (!chunk) {
	    (void)TryRead(TwD, TRUE);
	    answ = GetQueue(TwD, QREAD, &chunk);
	}
	if (chunk) {
	    if ((nul = memchr(answ, '\0', chunk))) {
		if (nul == answ && nul + 1 < answ + chunk) {
		    DeQueue(TwD, QREAD, 2);
		    answ = (byte *)(size_t)nul[1];
		    break;
		}
		chunk = nul - answ;
	    }
	    DeQueue(TwD, QREAD, chunk);
	    *len = chunk;
	    break;
	}
    } while (0);
#ifdef CONF_SOCKET_GZ
    GzipFlag = wasGzipFlag;
#endif
    
    UNLK;
    return answ;
}

/* this requires LOCK not to be held */
/**
 * tells the server to confirm the Tw_Attach() previously issued
 */
void Tw_AttachConfirm(tw_d TwD) {
    LOCK;
    if (Fd != TW_NOFD) {
	write(Fd, "\1", 1);
    }
    UNLK;
}


/* this requires LOCK not to be held */
/**
 * returns a pointer to last error information (libTw equivaled of errno_location())
 */
tw_errno *Tw_ErrnoLocation(tw_d TwD) {
    s_tw_errno *t;
    if (TwD) {
	LOCK; t = GetErrnoLocation(TwD); UNLK;
    } else
	t = &rCommonErrno;
    return (tw_errno *)t;
}

/**
 * returns a string description of given error
 */
TW_FNATTR_CONST TW_CONST byte *Tw_StrError(TW_CONST tw_d TwD, uldat e) {
    switch (e) {
      case 0:
	return "success";
      case TW_EX_ENDIAN:
	return "server has reversed endianity, impossible to connect";
      case TW_EX_SIZES:
	return "server has different data sizes, impossible to connect";
      case TW_ELOST_CONN:
	return "connection lost (explicit kill or server shutdown)";
      case TW_EALREADY_CONN:
	return "already connected";
      case TW_ENO_DISPLAY:
	return "TWDISPLAY is not set";
      case TW_EBAD_DISPLAY:
	return "badly formed TWDISPLAY";
      case TW_ECANT_CONN:
	return "failed to connect: ";
      case TW_ENO_MEM:
	return "out of memory";
      case TW_ECANT_WRITE:
	return "failed to send data to server: ";
      case TW_ENO_FUNCTION:
	return "function not supported by server: ";
      case TW_ESTRANGE:
	return "got strange data from server, protocol violated";
      case TW_ENO_AUTH:
	return "bad or missing authorization file ~/.TwinAuth, cannot connect";
      case TW_EDENIED:
	return "server denied permission to connect, file ~/.TwinAuth may be wrong";
      case TW_EBAD_GZIP:
	return "got invalid data from server, gzip format violated";
      case TW_EINTERNAL_GZIP:
	return "internal gzip error, panic!";
      case TW_ENO_HOST:
	return "unknown host in TWDISPLAY: ";
      case TW_EBAD_FUNCTION:
	return "function is not a possible server function";
      case TW_EX_PROTOCOL:
	return "server has incompatible protocol version, impossible to connect";
      case TW_ENO_SOCKET:
	return "failed to create socket: ";
      case TW_ESTRANGE_CALL:
	return "server function call returned strange data, wrong data sizes? : ";
      case TW_EFAILED_CALL:
	return "function call rejected by server, wrong data sizes? : ";
      case TW_EFAILED_ARG_CALL:
	return "function call rejected by server, invalid arguments? : ";
      case TW_EXLIB_SIZES:
	return "compiled data sizes are incompatible with libTw now in use!";
      default:
	return "unknown error";
    }
}

/**
 * returns a string description of given error detail
 */
TW_FNATTR_CONST TW_CONST byte *Tw_StrErrorDetail(TW_CONST tw_d TwD, uldat E, uldat S) {
    switch (E) {
      case TW_ECANT_CONN:
      case TW_ECANT_WRITE:
      case TW_ENO_SOCKET:
	return strerror(S);
      case TW_ENO_HOST:
	return hstrerror(S);
      case TW_ENO_FUNCTION:
      case TW_ESTRANGE_CALL:
      case TW_EFAILED_CALL:
      case TW_EFAILED_ARG_CALL:
	return Functions[S].name;
    }
    return "";
}

/* this requires LOCK not to be held */
/**
 * returns the file descriptor used by the connection
 */
int Tw_ConnectionFd(tw_d TwD) {
    int f;
    LOCK; f = Fd; UNLK;
    return f;
}

/* hack:
 * TwReadMsg() returns an already DeQueued tmsg.
 * TwPeekMsg() returns a tmsg without DeQueueing it.
 * They work because DeQueue() doesn't touch the data, just sets
 * some variables to mark it free.
 * But if you AddQueueAligned(QMSG) you may junk your precious tmsg.
 * There is no easy solution to this, except for Tw_CloneReadMsg() it,
 * as both DeQueued and still queued tmsg may be moved around
 * by the TwReallocMem() in AddQueueAligned(QMSG) in ParseReplies().
 * 
 * So: the contents of the tmsg returned by TwReadMsg()
 * becomes undefined after a call to a function that internally
 * calls ParseReplies(). These "bad" functions are:
 * 
 * TwSync();
 * TwPeekMsg();
 * TwReadMsg();
 * TwCreateGadget();
 * TwSearchGadget();
 * TwCreateWindow();
 * TwCreate4MenuWindow();
 * TwCreate4MenuMenuItem();
 * TwCreateMsgPort();
 * 
 * and in general any Tw() function of libTw.h returning non-void
 * so that it sends something to the server and waits for the server
 * to send back the return value.
 */

/* this requires LOCK to be held */
static tmsg ReadMsg(tw_d TwD, byte Wait, byte deQueue) {
    tmsg Msg = (tmsg)0;
    uldat len;

    if (Fd != TW_NOFD) {
	Msg = (tmsg)GetQueue(TwD, QMSG, &len);
    
	if (!len) {
	    Flush(TwD, Wait);
	    do {
		if (TryRead(TwD, Wait) != (uldat)-1) {
		    ParseReplies(TwD);
		    Msg = (tmsg)GetQueue(TwD, QMSG, &len);
		}
	    } while (Wait && Fd != TW_NOFD && !len);
	}
    
	if (Fd != TW_NOFD && len) {
	    if (deQueue)
		DeQueueAligned(TwD, QMSG, Msg->Len);
	} else
	    Msg = (tmsg)0;
    }
    return Msg;
}

/* this requires LOCK not to be held */
/**
 * returns the first tmsg already received, or NULL if no events
 * are available in the queue; it never blocks nor reads from the connection
 */
tmsg Tw_PendingMsg(tw_d TwD) {
    tmsg Msg;
    uldat len;
    LOCK; Msg = (tmsg)GetQueue(TwD, QMSG, &len); UNLK;
    if (!len)
	Msg = (tmsg)0;
    return Msg;
}

/* non-blocking check if there are messages available,
 * reading non-blocking from the socket if necessary */
/* this requires LOCK not to be held */
/**
 * returns the first tmsg already received, or tries to read non-blocking
 * more messages from the connection; if no messages are available,
 * returns NULL
 */
tmsg Tw_PeekMsg(tw_d TwD) {
    tmsg Msg;
    LOCK; Msg = ReadMsg(TwD, FALSE, FALSE); UNLK;
    return Msg;
}

/**
 * returns the first tmsg already received, or tries to read non-blocking
 * more messages from the connection; if Wait is TRUE and no messages are
 * immediately available, blocks until a message is received
 * and returns NULL only in case of a fatal error (panic);
 * 
 * in any case, the tmsg returned is removed from the queue.
 */
tmsg Tw_ReadMsg(tw_d TwD, byte Wait) {
    tmsg Msg;
    LOCK; Msg = ReadMsg(TwD, Wait, TRUE); UNLK;
    return Msg;
}

/**
 * behaves exactly like Tw_ReadMsg(), but a Tw_AllocMem()ed copy of the message
 * is returned to avoid concurrency problems with other threads;
 * you must Tw_FreeMem() it when done!
 */
tmsg Tw_CloneReadMsg(tw_d TwD, byte Wait) {
    tmsg Msg, ClonedMsg = (tmsg)0;
    LOCK;
    if ((Msg = ReadMsg(TwD, Wait, TRUE)) &&
	(ClonedMsg = (tmsg)Tw_AllocMem(Msg->Len))) {
	
	Tw_CopyMem(Msg, ClonedMsg, Msg->Len);
    }
    UNLK;
    return ClonedMsg;
}

/* AVL listeners tree handling functions */

TW_INLINE uldat TwAVLgetkey(uldat Type, tevent_common Event) {
    /* only (udat)Type is significative, not whole Type. */
    return (Type << 5) ^ Event->W ^ ((uldat)Event->Code << ((sizeof(uldat) - sizeof(udat)) * 8));
}

#define TwAVLgetkeyMsg(Msg) TwAVLgetkey((Msg)->Type, &(Msg)->Event.EventCommon)

#define TwAVLgetkeyListener(L) TwAVLgetkey((L)->Type, &(L)->Event->EventCommon)
  
/* this assumes L1->AVLkey == L2->AVLkey */
static int CompareListeners(tlistener L1, tlistener L2) {
    tevent_any L1A = L1->Event, L2A = L2->Event;
    
    if (L1->Type == L2->Type) {
	if (L1A->EventCommon.W == L2A->EventCommon.W) {
	    if (L1A->EventCommon.Code == L2A->EventCommon.Code) {
		/* common part matches. check details. */
		switch (L1->Type) {
		    /*
		     * WARNING: I am assuming all fields of a union
		     * have the same address as the union itself
		     */
		  case TW_MSG_WIDGET_KEY:
		  case TW_MSG_WIDGET_MOUSE:
		    return L2A->EventKeyboard.ShiftFlags - L1A->EventKeyboard.ShiftFlags;
		  case TW_MSG_MENU_ROW:
		    return L2A->EventMenu.Menu - L1A->EventMenu.Menu;
		  case TW_MSG_WIDGET_CHANGE:
		  case TW_MSG_WIDGET_GADGET:
		  case TW_MSG_SELECTION:
		  case TW_MSG_SELECTIONNOTIFY:
		  case TW_MSG_SELECTIONREQUEST:
		  case TW_MSG_SELECTIONCLEAR:
		  case TW_MSG_USER_CONTROL:
		    /* no extra checks needed */
		  default:
		    break;
		}
		return 0;
	    }
	    return L2A->EventCommon.Code - L1A->EventCommon.Code;
	}
	return L2A->EventCommon.W - L1A->EventCommon.W;
    }
    return L2->Type - L1->Type;
}

static tlistener FindListener(tw_d TwD, tmsg Msg) {
    struct s_tlistener key;
    
    key.Type = Msg->Type;
    key.Event = &Msg->Event;
    key.AVLkey = TwAVLgetkeyMsg(Msg);
    
    return (tlistener)AVLFind((tavl)&key, (tavl)TwD->AVLRoot, (tavl_compare)CompareListeners);
}

static void InsertListener(tw_d TwD, tlistener L) {
    if (L && !L->TwD) {
	L->AVLkey = TwAVLgetkeyListener(L);
	L->TwD = TwD;
	AVLInsert((tavl)L, (tavl)TwD->AVLRoot, (tavl_compare)CompareListeners, (tavl *)&TwD->AVLRoot);
    }
}

static void RemoveListener(tw_d TwD, tlistener L) {
    if (L && L->TwD == TwD) {
	AVLRemove((tavl)L, (tavl_compare)CompareListeners, (tavl *)&TwD->AVLRoot);
	L->TwD = NULL;
    }
}

static void DeleteAllListeners(tlistener L) {
    if (L) {
	DeleteAllListeners(L->Left);
	DeleteAllListeners(L->Right);
	if (L->Event)
	    Tw_FreeMem(L->Event);
	Tw_FreeMem(L);
    }
}

/**
 * adds an event listener to connection; event listeners are used only
 * if you call Tw_MainLoop() or Tw_DispatchMsg()
 */
void Tw_InsertListener(tw_d TwD, tlistener L) {
    LOCK;
    InsertListener(TwD, L);
    UNLK;
}

/**
 * removes an event listener from connection
 */
void Tw_RemoveListener(tw_d TwD, tlistener L) {
    LOCK;
    RemoveListener(TwD, L);
    UNLK;
}

/**
 * deletes an event listener
 */
void Tw_DeleteListener(tw_d TwD, tlistener L) {
    LOCK;
    if (L->TwD == TwD) {
	RemoveListener(TwD, L);
	if (L->Event)
	    Tw_FreeMem(L->Event);
	Tw_FreeMem(L);
    }
    UNLK;
}

/**
 * sets the fallback event listener, to be called when no other listeners match
 */
void Tw_SetDefaultListener(tw_d TwD, tfn_default_listener Listener, void *Arg) {
    LOCK;
    TwD->DefaultListener = Listener;
    TwD->DefaultArg = Arg;
    UNLK;
}


static tlistener CreateListener(tw_d TwD, udat Type, tevent_any E,
				tfn_listener Listener, void *Arg) {
    tlistener L;
    if ((L = (tlistener)Tw_AllocMem(sizeof(struct s_tlistener)))) {
	L->AVLParent = NULL;
	L->Type = Type;
	L->Event = E;
	L->Listener = Listener;
	L->Arg = Arg;
	L->TwD = NULL;
	LOCK;
	InsertListener(TwD, L);
	UNLK;
    }
    return L;
}

/* this does NOT add the created tlistener to the display! */
/**
 * creates an event listener; you must manually Tw_InsertListener() it
 */
tlistener Tw_CreateListener(tw_d TwD, udat Type, tevent_any E,
			    tfn_listener Listener, void *Arg) {
    tlistener L;
    if ((L = (tlistener)Tw_AllocMem(sizeof(struct s_tlistener)))) {
	L->AVLParent = NULL;
	L->Type = Type;
	L->Left = L->Right = NULL;
	L->Event = E;
	L->Listener = Listener;
	L->Arg = Arg;
	L->TwD = NULL;
    }
    return L;
}

/**
 * changes the already inserted event listener to listen for
 * the new Type and event
 */
void Tw_SetTEListener(tw_d TwD, tlistener L, udat Type, tevent_any E) {
    LOCK;
    if (L->TwD && L->TwD == TwD) {
	RemoveListener(TwD, L);
	L->Type = Type;
	L->Event = E;
	InsertListener(TwD, L);
    } else if (!L->TwD) {
	L->Type = Type;
	L->Event = E;
    }
    UNLK;
}

static tevent_any CreateEvent(twidget W, udat Code, udat ShiftFlags) {
    
    tevent_common E;
    
    if ((E = (tevent_common)Tw_AllocMem(sizeof(struct s_tevent_common)))) {
	E->W = W;
	E->Code = Code;
	E->pad = ShiftFlags;
	/* warning: we RELY on tevent_keyboard->ShiftFlags
	 * to be the same as tevent_common->pad */
    }
    return (tevent_any)E;
}

static tevent_any CreateMenuEvent(twidget W, tmenu Menu, udat Code) {
    
    tevent_menu E;
    
    if ((E = (tevent_menu)Tw_AllocMem(sizeof(struct s_tevent_menu)))) {
	E->W = W;
	E->Code = Code;
	E->Menu = Menu;
    }
    return (tevent_any)E;
}

static tlistener AddListener(tw_d TwD, udat Type, twidget W, udat Code, udat ShiftFlags,
			tfn_listener Listener, void *Arg) {
    
    tlistener L;
    tevent_any E;
    
    if ((E = CreateEvent(W, Code, ShiftFlags))) {
	if ((L = CreateListener(TwD, Type, E, Listener, Arg)))
	    return L;
	Tw_FreeMem(E);
    }
    return NULL;
}

static tlistener AddMenuListener(tw_d TwD, udat Type, twidget W, tmenu Menu, udat Code,
				 tfn_listener Listener, void *Arg) {
    
    tlistener L;
    tevent_any E;
    
    if ((E = CreateMenuEvent(W, Menu, Code))) {
	if ((L = CreateListener(TwD, Type, E, Listener, Arg)))
	    return L;
	Tw_FreeMem(E);
    }
    return NULL;
}

/**
 * creates and adds a keyboard event listener
 */
tlistener Tw_AddKeyboardListener(tw_d TwD, twidget W, udat Code, udat ShiftFlags,
				 tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_WIDGET_KEY, W, Code, ShiftFlags, Listener, Arg);
}

/**
 * creates and adds a mouse event listener
 */
tlistener Tw_AddMouseListener(tw_d TwD, twidget W, udat Code, udat ShiftFlags,
			      tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_WIDGET_MOUSE, W, Code, ShiftFlags, Listener, Arg);
}

/**
 * creates and adds a control message listener
 */
tlistener Tw_AddControlListener(tw_d TwD, twidget W, udat Code,
				tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_USER_CONTROL, W, Code, 0, Listener, Arg);
}

/**
 * creates and adds a client message listener
 */
tlistener Tw_AddClientMsgListener(tw_d TwD, twidget W, udat Code,
				  tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_USER_CLIENTMSG, W, Code, 0, Listener, Arg);
}

/**
 * creates and adds a `display' message listener
 * (the ones used by twdisplay to implement remote display hw)
 */
tlistener Tw_AddDisplayListener(tw_d TwD, udat Code,
				tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_DISPLAY, TW_NOID, Code, 0, Listener, Arg);
}

/**
 * creates and adds a widget change message listener
 */
tlistener Tw_AddWidgetListener(tw_d TwD, twidget W, udat Code,
			       tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_WIDGET_CHANGE, W, Code, 0, Listener, Arg);
}

/**
 * creates and adds a gadget message listener
 */
tlistener Tw_AddGadgetListener(tw_d TwD, twidget W, udat Code,
			       tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_WIDGET_GADGET, W, Code, 0, Listener, Arg);
}

/**
 * creates and adds a menu activation message listener
 */
tlistener Tw_AddMenuListener(tw_d TwD, twidget W, tmenu Menu, udat Code,
			     tfn_listener Listener, void *Arg) {
    return AddMenuListener(TwD, TW_MSG_MENU_ROW, W, Menu, Code, Listener, Arg);
}

/**
 * creates and adds a selection message listener
 */
tlistener Tw_AddSelectionListener(tw_d TwD, twidget W,
				  tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_SELECTION, W, 0, 0, Listener, Arg);
}

/**
 * creates and adds a selection-notify message listener
 */
tlistener Tw_AddSelectionNotifyListener(tw_d TwD, tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_SELECTIONNOTIFY, TW_NOID, 0, 0, Listener, Arg);
}

/**
 * creates and adds a selection-request message listener
 */
tlistener Tw_AddSelectionRequestListener(tw_d TwD, tfn_listener Listener, void *Arg) {
    return AddListener(TwD, TW_MSG_SELECTIONREQUEST, TW_NOID, 0, 0, Listener, Arg);
}


static byte DispatchMsg(tdisplay TwD, tmsg Msg, byte mustClone) {
    tlistener L;
    tfn_listener Listener;
    tfn_default_listener DefaultListener;
    void *Arg;
    tmsg ClonedMsg;

    if ((L = FindListener(TwD, Msg))) {
	Listener = L->Listener;
	Arg = L->Arg;
	DefaultListener = NULL;
    } else if (TwD->DefaultListener) {
	DefaultListener = TwD->DefaultListener;
	Arg = TwD->DefaultArg;
	Listener = NULL;
    } else
	return FALSE;

    /*
     * Tw_MainLoop calls us with a Msg still inside Queue[].
     * Clone it before calling Listener to avoid problems.
     * 
     * If the user calls Tw_DispatchMsg, it's up to him to ensure
     * there are no problems (possibly using Tw_CloneReadMsg() instead
     * of Tw_ReadMsg() ).
     */
    if (mustClone) {
	if ((ClonedMsg = (tmsg)Tw_AllocMem(Msg->Len)))
	    Tw_CopyMem(Msg, ClonedMsg, Msg->Len);
    } else
	ClonedMsg = Msg;
    
    if (ClonedMsg) {
	UNLK;
	if (Listener)
	    Listener(&ClonedMsg->Event, Arg);
	else
	    DefaultListener(ClonedMsg, Arg);
	LOCK;
	if (mustClone)
	    Tw_FreeMem(ClonedMsg);
	return TRUE;
    }
    return FALSE;
}

/**
 * calls the appropriate event listener for given tmsg
 */
byte Tw_DispatchMsg(tdisplay TwD, tmsg Msg) {
    byte ret;
    LOCK;
    ret = DispatchMsg(TwD, Msg, FALSE);
    UNLK;
    return ret;
}

/**
 * utility function: sits in a loop, waiting for messages and calling
 * the appropriate event listener for them, until a fatal error occurs
 */
byte Tw_MainLoop(tw_d TwD) {
    byte ret;
    tmsg Msg;
    
    LOCK;
    Errno = 0;
    while ((Msg = ReadMsg(TwD, TRUE, TRUE)))
	(void)DispatchMsg(TwD, Msg, TRUE);	
    ret = Errno == 0;
    UNLK;
    return ret;
}





static uldat FindFunctionId(tw_d TwD, uldat order);

static void FailedCall(tw_d TwD, uldat err, uldat order) {
    s_tw_errno *vec = GetErrnoLocation(TwD);
    vec->E = err;
    vec->S = order;
}

TW_INLINE uldat NextSerial(tw_d TwD) {
    if (++RequestN == msg_magic)
	++RequestN;
    return RequestN;
}

TW_INLINE void Send(tw_d TwD, uldat Serial, uldat idFN) {
    /* be careful with aligmnent!! */
#ifdef __i386__
    r[0] = s - (byte *)(r+1);
    r[1] = Serial;
    r[2] = idFN;
#else
    byte *R = (byte *)r;
    Push(R, uldat, s - (byte *)(r+1));
    Push(R, uldat, Serial);
    Push(R, uldat, idFN);
#endif
}

/***********/


#include "libTw2m4.h"

/* handy special cases (also for compatibility) */
/**
 * sets a gadget to pressed or unpressed
 */
void Tw_SetPressedGadget(tw_d TwD, tgadget a1, byte a2) {
    Tw_ChangeField(TwD, a1, TWS_gadget_Flags, TW_GADGETFL_PRESSED, a2 ? TW_GADGETFL_PRESSED : 0);
}

/**
 * returns wether a gadget is pressed or not
 */
byte Tw_IsPressedGadget(tw_d TwD, tgadget a1) {
    return Tw_Stat(TwD, a1, TWS_gadget_Flags) & TW_GADGETFL_PRESSED ? TRUE : FALSE;
}

/**
 * sets a gadget to toggle-type or to normal type
 */
void Tw_SetToggleGadget(tw_d TwD, tgadget a1, byte a2) {
    Tw_ChangeField(TwD, a1, TWS_gadget_Flags, TW_GADGETFL_TOGGLE, a2 ? TW_GADGETFL_TOGGLE : 0);
}

/**
 * return wether a gadget is toggle-type or not
 */
byte Tw_IsToggleGadget(tw_d TwD, tgadget a1) {
    return Tw_Stat(TwD, a1, TWS_gadget_Flags) & TW_GADGETFL_TOGGLE ? TRUE : FALSE;
}

/**
 * draws given portion of a widget; usually called after a TW_WIDGET_EXPOSE message
 */
void Tw_ExposeWidget2(tw_d TwD, twidget a1, dat a2, dat a3, dat a4, dat a5, dat pitch, TW_CONST byte *a6, TW_CONST hwfont *a7, TW_CONST hwattr *a8) {
    uldat len6;
    uldat len7;
    uldat len8;
    uldat My;
    LOCK;
    if (Fd != TW_NOFD && ((My = id_Tw[order_ExposeWidget]) != TW_NOID ||
		       (My = FindFunctionId(TwD, order_ExposeWidget)) != TW_NOID)) {
	if (InitRS(TwD)) {
            My = (0 + sizeof(uldat) + sizeof(dat) + sizeof(dat) + sizeof(dat) + sizeof(dat) + (len6 = a6 ? (a2*a3) * sizeof(byte) : 0, sizeof(uldat) + len6) + (len7 = a7 ? (a2*a3) * sizeof(hwfont) : 0, sizeof(uldat) + len7) + (len8 = a8 ? (a2*a3) * sizeof(hwattr) : 0, sizeof(uldat) + len8) );
            if (WQLeft(My)) {
                Push(s,uldat,a1); Push(s,dat,a2); Push(s,dat,a3); Push(s,dat,a4); Push(s,dat,a5);
		Push(s,uldat,len6);
		while (len6) {
		    PushV(s,a2,a6);
		    a6 += pitch;
		    len6 -= a2;
		}
		Push(s,uldat,len7);
		while (len7) {
		    PushV(s,a2,a7);
		    a7 += pitch;
		    len7 -= a2;
		}
		Push(s,uldat,len8);
		while (len8) {
		    PushV(s,a2,a8);
		    a8 += pitch;
		    len8 -= a2;
		}
	            Send(TwD, (My = NextSerial(TwD)), id_Tw[order_ExposeWidget]);
                    UNLK;return;
            }
	}
	/* still here? must be out of memory! */
	Errno = TW_ENO_MEM;
	Fail(TwD);
    } else if (Fd != TW_NOFD)
	FailedCall(TwD, TW_ENO_FUNCTION, order_ExposeWidget);
    
    UNLK;
}

/**
 * draws given portion of a widget; usually called after a TW_WIDGET_EXPOSE message
 */
void Tw_ExposeTextWidget(tw_d TwD, twidget W, dat XWidth, dat YWidth, dat Left, dat Up, dat Pitch, TW_CONST byte *Text) {
    Tw_ExposeWidget2(TwD, W, XWidth, YWidth, Left, Up, Pitch, Text, NULL, NULL);
}

/**
 * draws given portion of a widget; usually called after a TW_WIDGET_EXPOSE message
 */
void Tw_ExposeHWFontWidget(tw_d TwD, twidget W, dat XWidth, dat YWidth, dat Left, dat Up, dat Pitch, TW_CONST hwfont *Font) {
    Tw_ExposeWidget2(TwD, W, XWidth, YWidth, Left, Up, Pitch, NULL, Font, NULL);
}

/**
 * draws given portion of a widget; usually called after a TW_WIDGET_EXPOSE message
 */
void Tw_ExposeHWAttrWidget(tw_d TwD, twidget W, dat XWidth, dat YWidth, dat Left, dat Up, dat Pitch, TW_CONST hwattr *Attr) {
    Tw_ExposeWidget2(TwD, W, XWidth, YWidth, Left, Up, Pitch, NULL, NULL, Attr);
}



/* handy aliases (also for completeness) */
/**
 * sends all buffered data to connection and waits for server to process it
 */
byte Tw_SyncSocket(tw_d TwD)				{ return Tw_Sync(TwD); }


/**
 * sets given portion of gadget's contents
 */
void Tw_WriteTextGadget(tw_d TwD, tgadget G, dat Width, dat Height, TW_CONST byte *Text, dat Left, dat Up) {
    Tw_WriteTextsGadget(TwD, G, 1, Width, Height, Text, Left, Up);
}

/**
 * clears whole gadget contents, then sets given portion of gadget's contents
 */
void Tw_SetTextGadget(tw_d TwD, tgadget G, dat Width, dat Height, TW_CONST byte *Text, dat Left, dat Up) {
    /* clear the whole gadget */
    Tw_WriteTextsGadget(TwD, G, 1, MAXDAT, MAXDAT, NULL, 0, 0);
    /* write the specified text */
    Tw_WriteTextsGadget(TwD, G, 1, Width, Height, Text, Left, Up);
}

/**
 * clears whole gadget contents, then sets given portion of gadget's contents
 */
void Tw_SetTextsGadget(tw_d TwD, tgadget G, byte bitmap, dat Width, dat Height, TW_CONST byte *Text, dat Left, dat Up) {
    /* clear the whole gadget */
    Tw_WriteTextsGadget(TwD, G, bitmap, MAXDAT, MAXDAT, NULL, 0, 0);
    /* write the specified text */
    Tw_WriteTextsGadget(TwD, G, bitmap, Width, Height, Text, Left, Up);
}


/**
 * sets given portion of gadget's contents
 */
void Tw_WriteHWFontGadget(tw_d TwD, tgadget G, dat Width, dat Height, TW_CONST hwfont *HWFont, dat Left, dat Up) {
    Tw_WriteHWFontsGadget(TwD, G, 1, Width, Height, HWFont, Left, Up);
}

/**
 * clears whole gadget contents, then sets given portion of gadget's contents
 */
void Tw_SetHWFontGadget(tw_d TwD, tgadget G, dat Width, dat Height, TW_CONST hwfont *HWFont, dat Left, dat Up) {
    /* clear the whole gadget */
    Tw_WriteHWFontsGadget(TwD, G, 1, MAXDAT, MAXDAT, NULL, 0, 0);
    /* write the specified text */
    Tw_WriteHWFontsGadget(TwD, G, 1, Width, Height, HWFont, Left, Up);
}

/**
 * clears whole gadget contents, then sets given portion of gadget's contents
 */
void Tw_SetHWFontsGadget(tw_d TwD, tgadget G, byte bitmap, dat Width, dat Height, TW_CONST hwfont *HWFont, dat Left, dat Up) {
    /* clear the whole gadget */
    Tw_WriteHWFontsGadget(TwD, G, bitmap, MAXDAT, MAXDAT, NULL, 0, 0);
    /* write the specified text */
    Tw_WriteHWFontsGadget(TwD, G, bitmap, Width, Height, HWFont, Left, Up);
}


static byte Sync(tw_d TwD) {
    uldat left;
    return Fd != TW_NOFD ? (GetQueue(TwD, QWRITE, &left), left) ? _Tw_SyncSocket(TwD) : TRUE : FALSE;
}

/**
 * sends all buffered data to connection and waits for server to process it
 */
byte Tw_Sync(tw_d TwD) {
    byte left;
    LOCK; left = Sync(TwD); UNLK;
    return left;
}


/***********/


/* tslist flags: */
#define TWS_CLONE_MEM 1
#define TWS_SORTED    2
#define TWS_SCALAR    4


static uldat FindFunctionId(tw_d TwD, uldat order) {
    uldat My;
    if ((My = id_Tw[order]) == TW_NOID) {
	My = id_Tw[order] = _Tw_FindFunction
	    (TwD, Functions[order].len, 3 + Functions[order].name, Functions[order].formatlen, Functions[order].format+1);
    }
    return Fd != TW_NOFD ? My : TW_NOID;
}

static tslist AStat(tw_d TwD, tobj Id, udat flags, udat hN, TW_CONST udat *h, tsfield f);

/**
 * returns information about given object
 */
tlargest Tw_Stat(tw_d TwD, tobj Id, udat h) {
    struct s_tsfield f;
    AStat(TwD, Id, TWS_SCALAR, 1, &h, &f);
    return f.TWS_field_scalar;
}

/**
 * returns information about given object
 */
tslist Tw_LStat(tw_d TwD, tobj Id, udat hN, ...) {
    tslist TS;
    va_list ap;
    
    va_start(ap, hN);
    TS = Tw_VStat(TwD, Id, hN, ap);
    va_end(ap);
    
    return TS;
}
/**
 * returns information about given object
 */
tslist Tw_AStat(tw_d TwD, tobj Id, udat hN, TW_CONST udat *h) {
    return AStat(TwD, Id, 0, hN, h, NULL);
}
/**
 * returns information about given object
 */
tslist Tw_VStat(tw_d TwD, tobj Id, udat hN, va_list ap) {
    tslist TS = NULL;
    udat i, *h;
    
    if (hN && (h = Tw_AllocMem(hN * sizeof(udat)))) {
	for (i = 0; i < hN; i++)
	    h[i] = va_arg(ap, int);
	TS = AStat(TwD, Id, 0, hN, h, NULL);
	Tw_FreeMem(h);
    }
    return TS;
}

/**
 * returns information about given object
 */
tslist Tw_CloneStat(tw_d TwD, tobj Id, udat hN, ...) {
    tslist TS;
    va_list ap;
    
    va_start(ap, hN);
    TS = Tw_CloneVStat(TwD, Id, hN, ap);
    va_end(ap);
    
    return TS;
}
/**
 * returns information about given object
 */
tslist Tw_CloneAStat(tw_d TwD, tobj Id, udat hN, TW_CONST udat *h) {
    return AStat(TwD, Id, TWS_CLONE_MEM, hN, h, NULL);
}
/**
 * returns information about given object
 */
tslist Tw_CloneVStat(tw_d TwD, tobj Id, udat hN, va_list ap) {
    tslist TS = NULL;
    udat i, *h;
    
    if (hN && (h = Tw_AllocMem(hN * sizeof(udat)))) {
	for (i = 0; i < hN; i++)
	    h[i] = va_arg(ap, int);
	TS = AStat(TwD, Id, TWS_CLONE_MEM, hN, h, NULL);
	Tw_FreeMem(h);
    }
    return TS;
}

/**
 * deletes information about an object that was returned with one of
 * the Tw_Clone*Stat() functions
 */
void Tw_DeleteStat(tw_d TwD, tslist TSL) {
    if (TSL) {
	if (TSL->flags & TWS_CLONE_MEM) {
	    udat i;
	    tsfield f;
	    for (i = 0; i < TSL->N; i++) {
		f = TSL->TSF;
		if (f->type >= TWS_vec && (f->type & ~TWS_vec) < TWS_last && f->TWS_field_vecV)
		    Tw_FreeMem(f->TWS_field_vecV);
	    }
	}
	Tw_FreeMem(TSL);
    }
}

static int CompareTSF(TW_CONST tsfield f1, TW_CONST tsfield f2) {
    return (int)f1->hash - f2->hash;
}

/**
 * searches for a specific field in object information
 */
tsfield Tw_FindStat(tw_d TwD, tslist TSL, udat hash) {
    struct s_tsfield f;

    f.hash = hash;
    
    return (tsfield)bsearch(&f, TSL->TSF, TSL->N, sizeof(struct s_tsfield),
			    (int (*)(TW_CONST void *, TW_CONST void *))CompareTSF);
}

static void SortTSL(tslist TSL) {
    tsfield f, end;
    udat hash;
    
    if (!(TSL->flags & TWS_SORTED)) {
	TSL->flags |= TWS_SORTED;
	for (hash = 0, f = TSL->TSF, end = f + TSL->N; f < end; f++) {
	    if (hash > f->hash)
		/* not sorted */
		break;
	    hash = f->hash;
	}
	if (f < end)
	    qsort(TSL->TSF, TSL->N, sizeof(struct s_tsfield),
		  (int (*)(TW_CONST void *, TW_CONST void *))CompareTSF);
    }
}

static tslist StatScalar(tsfield f, byte *data, byte *end) {
    udat i, pad, N;

    Pop(data,udat,N);
    Pop(data,udat,pad);
    
    if (N == 1 && data + 2*sizeof(udat) <= end) {
	Pop(data,udat,i);
	Pop(data,udat,pad);
	switch (i) {
# define Popcase(type) case TWS_CAT(TWS_,type): \
	    if (data + sizeof(type) <= end) { \
	        /* avoid padding problems */ \
		type tmp; \
		Pop(data,type,tmp); \
		f->TWS_field_scalar = tmp; \
	    } \
	    break
		    
	    Popcase(byte);
	    Popcase(dat);
	    Popcase(ldat);
	    Popcase(hwcol);
	    Popcase(time_t);
	    Popcase(frac_t);
	    Popcase(hwfont);
	    Popcase(hwattr);
	    Popcase(tobj);
#undef Popcase
	  default:
	    break;
	}
    }
    return (tslist)0;
}

static tslist StatTSL(tw_d TwD, udat flags, byte *data, byte *end) {
    tslist TSL;
    tsfield TSF;
    udat i, N;
    byte ok = TRUE;
    
    Pop(data,udat,N);
    Pop(data,udat,i); /* pad */
    
    if (N && (TSL = Tw_AllocMem(sizeof(struct s_tlist) + (N-1)*sizeof(struct s_tsfield)))) {
	TSL->N = N;
	TSL->flags = flags;
	TSF = TSL->TSF;
	for (i = 0; i < N && ok && data + 2*sizeof(udat) <= end; i++) {
	    Pop(data,udat,TSF[i].hash);
	    Pop(data,udat,TSF[i].type);
	    switch (TSF[i].type) {
# define Popcase(type) case TWS_CAT(TWS_,type): \
		if (data + sizeof(type) <= end) { \
	            /* avoid padding problems */ \
		    type tmp; \
		    Pop(data,type,tmp); \
		    TSF[i].TWS_field_scalar = tmp; \
		} else \
		    ok = FALSE; \
		break
		
		Popcase(byte);
		Popcase(dat);
		Popcase(ldat);
		Popcase(hwcol);
		Popcase(time_t);
		Popcase(frac_t);
		Popcase(hwfont);
		Popcase(hwattr);
		Popcase(tobj);
#undef Popcase
	      default:
		if (TSF[i].type >= TWS_vec && (TSF[i].type & ~TWS_vec) <= TWS_last &&
		    data + sizeof(uldat) <= end) {
		    
		    Pop(data,uldat,TSF[i].TWS_field_vecL);
		    if (TSF[i].TWS_field_vecL) {
			if (data + TSF[i].TWS_field_vecL <= end) {
			    if (flags & TWS_CLONE_MEM) {
				if ((TSF[i].TWS_field_vecV = Tw_AllocMem(TSF[i].TWS_field_vecL)))
				    PopV(data, TSF[i].TWS_field_vecL, TSF[i].TWS_field_vecV);
				else
				    ok = FALSE;
			    } else
				PopAddr(data, byte, TSF[i].TWS_field_vecL, TSF[i].TWS_field_vecV);
			} else
			    ok = FALSE;
		    } else
			TSF[i].TWS_field_vecV = NULL;
		} else
		    ok = FALSE;
		break;
	    }
	}
	if (ok && data == end /* paranoia */) {
	    SortTSL(TSL);
	    return TSL;
	}
	FailedCall(TwD, TW_ESTRANGE_CALL, order_StatObj);
	Tw_DeleteStat(/*cheat!*/ NULL, TSL);
    }
    return (tslist)0;
}

static tslist AStat(tw_d TwD, tobj Id, udat flags, udat hN, TW_CONST udat *h, tsfield f) {
    tslist a0;
    DECL_MyReply
    uldat My;
    LOCK;
    if (Fd != TW_NOFD && ((My = id_Tw[order_StatObj]) != TW_NOID ||
		       (My = FindFunctionId(TwD, order_StatObj)) != TW_NOID)) {
	if (InitRS(TwD)) {
            My = (sizeof(tobj) + sizeof(udat) + hN * sizeof(udat));
	    if (WQLeft(My)) {
		Push(s, tobj, Id); 
		Push(s, udat, hN);
		PushV(s, hN * sizeof(udat), h);

		Send(TwD, (My = NextSerial(TwD)), id_Tw[order_StatObj]);
		if ((MyReply = (void *)Wait4Reply(TwD, My)) && (INIT_MyReply MyCode == OK_MAGIC)) {
		    if (flags & TWS_SCALAR)
			a0 = StatScalar(f, (byte *)MyData, (byte *)MyReply + MyLen + sizeof(uldat));
		    else
			a0 = StatTSL(TwD, flags, (byte *)MyData, (byte *)MyReply + MyLen + sizeof(uldat));
		} else {
		    FailedCall(TwD, MyReply && MyCode != (uldat)-1 ?
			       TW_EFAILED_ARG_CALL : TW_EFAILED_CALL, order_StatObj);
		    a0 = (tslist)TW_NOID;
		}
		if (MyReply)
		    KillReply(TwD, (byte *)MyReply, MyLen);
		UNLK;
		return a0;
	    }
	}
	/* still here? must be out of memory! */
	Errno = TW_ENO_MEM;
	Fail(TwD);
    } else if (Fd != TW_NOFD)
	FailedCall(TwD, TW_ENO_FUNCTION, order_StatObj);
    UNLK;
    return (tslist)TW_NOID;
}










/***********/

/* wrap _Tw_FindFunction() with LOCKs */
/**
 * returns the server-dependant order number of a function (specified as string),
 * or NOID if server is missing that function
 */
uldat Tw_FindFunction(tw_d TwD, byte Len, TW_CONST byte *Name, byte FormatLen, TW_CONST byte *Format) {
    uldat MyId;
    LOCK; MyId = _Tw_FindFunction(TwD, Len, Name, FormatLen, Format); UNLK;
    return MyId;
}




static void ParseReplies(tw_d TwD) {
    uldat left, len, rlen, serial;
    byte *t;
    byte *rt;
    
    t = GetQueue(TwD, QREAD, &len);
    left = len;
    
    /* parse all replies, move messages to QMSG queue, delete malformed replies */
    while (left >= sizeof(uldat)) {
	rt = t;
	Pop(t,uldat,rlen);
	left -= sizeof(uldat);
	if (left >= rlen) {
	    if (rlen < 2*sizeof(uldat) || (Pop(t, uldat, serial), t -= sizeof(uldat), serial == MSG_MAGIC)) {
		
		/* either a MSG, or a malformed reply. In both cases, it will be removed */
		if (rlen >= 2*sizeof(uldat) && serial == MSG_MAGIC) {
		    /* it's a Msg, copy it in its own queue */
		    /* we no longer need `t', clobber it */
		    t -= sizeof(uldat);
		    Push(t, uldat, rlen + sizeof(uldat));
		    ParanoidAddQueueQMSG(TwD, rlen + sizeof(uldat), rt);
		}
		    
		/* delete the reply and recalculate t, left, len */
		KillReply(TwD, rt, rlen);
		(void)GetQueue(TwD, QREAD, &rlen);
		if (rlen) {
		    /* t -= sizeof(uldat); */
		    left += sizeof(uldat);
		    t = GetQueue(TwD, QREAD, NULL) + (len - left);
		    left -= len - rlen;
		    len = rlen;
		    continue;
		}
		return;
	    }
	    t += rlen;
	    left -= rlen;
	} else {
	    /* the last reply is still incomplete, do not touch it and bail out */
	    /* left += sizeof(uldat); */
	    break;
	}
    }
}

#define Delta ((uldat)(size_t)&(((tmsg)0)->Event))

/* nothing to lock here... */
/**
 * creates an event message
 */
tmsg Tw_CreateMsg(tw_d TwD, uldat Type, uldat EventLen) {
    tmsg Msg;

    if ((Msg = (tmsg)Tw_AllocMem(EventLen += Delta))) {
	Msg->Len = EventLen;
	Msg->Magic = msg_magic;
	Msg->Type = Type;
    }
    return Msg;
}

/**
 * deletes an event message: should be called only on messages
 * created with Tw_CreateMsg(), and only if they are not sent with
 * Tw_SendMsg() or Tw_BlindSendMsg()
 */
void Tw_DeleteMsg(tw_d TwD, tmsg Msg) {
    if (Msg && Msg->Magic == msg_magic)
	Tw_FreeMem(Msg);
}

/**
 * sends message to given client, blocking to see if it could be delivered
 */
byte Tw_SendMsg(tw_d TwD, tmsgport MsgPort, tmsg Msg) {
    byte ret = FALSE;
    if (Msg && Msg->Magic == msg_magic) {
	ret = Tw_SendToMsgPort(TwD, MsgPort, Msg->Len, (void *)Msg);
	Tw_FreeMem(Msg);
    }
    return ret;
}

/**
 * sends message to given client, without blocking
 */
void Tw_BlindSendMsg(tw_d TwD, tmsgport MsgPort, tmsg Msg) {
    if (Msg && Msg->Magic == msg_magic) {
	Tw_BlindSendToMsgPort(TwD, MsgPort, Msg->Len, (void *)Msg);
	Tw_FreeMem(Msg);
    }
}


    

/**
 * returns TRUE if server implements all given functions, FALSE otherwise
 */
byte Tw_FindFunctions(tw_d TwD, void *F, ...) {
    va_list L;
    void *tryF;
    uldat i, *id;
    s_tw_errno *E;

    if (F) {
	va_start(L, F);
	do {
	    for (i = 0; (tryF = Functions[i].Fn) && tryF != F; i++)
		;
	    if (tryF == F) {
		id = &id_Tw[i];
		if (*id != TW_NOID ||
		    (*id = FindFunctionId(TwD, i)) != TW_NOID)
		    
		    continue;
		E = GetErrnoLocation(TwD);
		E->E = TW_ENO_FUNCTION;
		E->S = i;
	    } else {
		Errno = TW_EBAD_FUNCTION;
	    }
	    va_end(L);
	    return FALSE;
	
	} while ((F = va_arg(L, void *)));

	va_end(L);
    }
    return TRUE;
}

#ifdef CONF_SOCKET_GZ

static voidpf Tw_ZAlloc(voidpf opaque, uInt items, uInt size) {
    void *ret = Tw_AllocMem(items * (size_t)size);
    return ret ? (voidpf)ret : Z_NULL;
}

static void Tw_ZFree(voidpf opaque, voidpf address) {
    if (address != Z_NULL)
	Tw_FreeMem((void *)address);
}

TW_INLINE byte *FillQueue(tw_d TwD, byte i, uldat *len) {
    uldat delta = Qmax[i] - Qlen[i] - Qstart[i];
    Qlen[i] += delta;
    if (len) *len = delta;
    return Queue[i] + Qstart[i] + Qlen[i] - delta;
}

/* compress data before sending */
static uldat Gzip(tw_d TwD) {
    uldat oldQWRITE = Qlen[QWRITE], delta, tmp;
    z_streamp z = zW;
    int zret = Z_OK;
    
    /* compress the queue */
    if (Qlen[QWRITE]) {
	z->next_in = GetQueue(TwD, QWRITE, &tmp); z->avail_in = tmp;
	z->next_out = FillQueue(TwD, QgzWRITE, &tmp); z->avail_out = tmp;
	    
	while (z->avail_in && zret == Z_OK) {
	    
	    if (z->avail_out < (delta = z->avail_in + 12)) {
		if (Grow(TwD, QgzWRITE, delta - z->avail_out)) {
		    Qlen[QgzWRITE] -= delta;
		    z->next_out = FillQueue(TwD, QgzWRITE, &tmp); z->avail_out = tmp;
		} else {
		    /* out of memory ! */
		    Errno = TW_ENO_MEM;
		    Panic(TwD);
		    break;
		}
	    }
	    
	    zret = deflate(z, Z_SYNC_FLUSH);

	    /* update the compressed queue */
	    Qlen[QgzWRITE] -= z->avail_out;
	}
    }
    
    if (Fd != TW_NOFD) {
	/* update the uncompressed queue */
	DeQueue(TwD, QWRITE, Qlen[QWRITE] - z->avail_in);
	
	if (zret == Z_OK)
	    return oldQWRITE;
	else {
	    Errno = TW_EINTERNAL_GZIP;
	    Panic(TwD);
	}
    }
    return FALSE;
}

static uldat Gunzip(tw_d TwD) {
    uldat oldQRead = Qlen[QREAD], delta, tmp;
    int zret = Z_OK;
    z_streamp z = zR;
    
    /* uncompress the queue */
    if (Qlen[QgzREAD]) {
	z->next_in = GetQueue(TwD, QgzREAD, &tmp); z->avail_in = tmp;
	z->next_out = FillQueue(TwD, QREAD, &tmp); z->avail_out = tmp;
	
	while (z->avail_in && zret == Z_OK) {
	    
	    /* approx. guess of uncompression ratio: 1 to 5 */
	    /* in case we guess wrong, inflate() will tell us to make more space */
	    if (z->avail_out < (delta = 5 * z->avail_in + 12)) {
		if (Grow(TwD, QREAD, delta - z->avail_out)) {
		    Qlen[QREAD] -= delta;
		    z->next_out = FillQueue(TwD, QREAD, &tmp); z->avail_out = tmp;
		} else {
		    /* out of memory ! */
		    Errno = TW_ENO_MEM;
		    Panic(TwD);
		    break;
		}
	    }
	    
	    zret = inflate(z, Z_SYNC_FLUSH);

	    /* update the uncompressed queue */
	    Qlen[QREAD] -= z->avail_out;
	}
    }
    
    if (Fd != TW_NOFD) {
	/* update the compressed queue */
	DeQueue(TwD, QgzREAD, Qlen[QgzREAD] - z->avail_in);
	
	if (zret == Z_OK)
	    return Qlen[QREAD] - oldQRead;
	else {
	    Errno = TW_EBAD_GZIP;
	    Panic(TwD);
	}
    }
    return FALSE;
}

/**
 * tries to enable compression on the connection; returns TRUE if succeeded
 */
byte Tw_EnableGzip(tw_d TwD) {
    if (!GzipFlag && Tw_CanCompress(TwD)) {
	if ((zW = Tw_AllocMem(sizeof(*zW))) &&
	    (zR = Tw_AllocMem(sizeof(*zR)))) {
	    
	    if (Tw_AllocMem == malloc) {
		zW->zalloc = zR->zalloc = Z_NULL;
		zW->zfree  = zR->zfree  = Z_NULL;
	    } else {
		zW->zalloc = zR->zalloc = Tw_ZAlloc;
		zW->zfree  = zR->zfree  = Tw_ZFree;
	    }
	    zW->opaque = zR->opaque = NULL;

	    if (deflateInit(zW, Z_BEST_COMPRESSION) == Z_OK) {
		if (inflateInit(zR) == Z_OK) {
		    if (Tw_DoCompress(TwD, TRUE))
			return GzipFlag = TRUE;
		    inflateEnd(zR);
		}
		deflateEnd(zW);
	    }
	}
	if (zR) Tw_FreeMem(zR);
	if (zW) Tw_FreeMem(zW);
    }
    return FALSE;
}

/**
 * tries to disable compression on the connection; returns TRUE if succeeded
 */
byte Tw_DisableGzip(tw_d TwD) {
    if (GzipFlag && (Fd == TW_NOFD || Tw_DoCompress(TwD, FALSE) || Fd == TW_NOFD)) {
	inflateEnd(zR);
	deflateEnd(zW);
	Tw_FreeMem(zR);
	Tw_FreeMem(zW);
	GzipFlag = FALSE;
	return TRUE;
    }
    return FALSE;
}

#else /* !CONF_SOCKET_GZ */

byte Tw_EnableGzip(tw_d TwD) {
    return FALSE;
}

byte Tw_DisableGzip(tw_d TwD) {
    return FALSE;
}

#endif /* CONF_SOCKET_GZ */
