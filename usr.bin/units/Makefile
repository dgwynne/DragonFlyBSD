# $FreeBSD: src/usr.bin/units/Makefile,v 1.5.2.1 2002/02/18 16:16:57 ru Exp $

PROG=	units
FILES=	units.lib
FILESDIR=	${SHAREDIR}/misc

LDADD+=	-lprivate_edit -lprivate_ncurses
DPADD+=	${LIBEDIT} ${LIBNCURSES}
LDFLAGS+=	${PRIVATELIB_LDFLAGS}

.include <bsd.prog.mk>
